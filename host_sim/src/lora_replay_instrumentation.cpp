#include "host_sim/lora_replay/instrumentation.hpp"

#include "host_sim/scheduler.hpp"
#include "host_sim/stages/demod_stage.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <utility>

namespace host_sim::lora_replay
{
namespace
{

class FileSymbolSource : public host_sim::SymbolSource
{
public:
    FileSymbolSource(std::vector<std::complex<float>> samples,
                     std::size_t alignment_offset,
                     std::size_t samples_per_symbol,
                     std::size_t symbol_count)
        : samples_(std::move(samples)),
          offset_(alignment_offset),
          samples_per_symbol_(samples_per_symbol),
          symbol_count_(symbol_count)
    {
    }

    void reset() override { index_ = 0; }

    std::optional<host_sim::SymbolBuffer> next_symbol() override
    {
        if (index_ >= symbol_count_) {
            return std::nullopt;
        }
        const auto start = offset_ + index_ * samples_per_symbol_;
        if (start + samples_per_symbol_ > samples_.size()) {
            return std::nullopt;
        }
        host_sim::SymbolBuffer buffer;
        buffer.samples.insert(buffer.samples.end(),
                              samples_.begin() + static_cast<std::ptrdiff_t>(start),
                              samples_.begin()
                                  + static_cast<std::ptrdiff_t>(start + samples_per_symbol_));
        ++index_;
        return buffer;
    }

private:
    std::vector<std::complex<float>> samples_;
    std::size_t offset_;
    std::size_t samples_per_symbol_;
    std::size_t symbol_count_;
    std::size_t index_{0};
};

} // namespace

std::size_t compute_samples_per_symbol(const host_sim::LoRaMetadata& meta)
{
    const std::size_t chips = static_cast<std::size_t>(1) << meta.sf;
    return static_cast<std::size_t>((static_cast<long long>(meta.sample_rate) * chips) / meta.bw);
}

InstrumentationResult run_scheduler_instrumentation(const std::vector<std::complex<float>>& samples,
                                                    const host_sim::LoRaMetadata& meta,
                                                    std::size_t alignment_offset,
                                                    std::size_t max_symbols,
                                                    double cycle_scale,
                                                    bool use_fixed_numeric,
                                                    const Options& options)
{
    InstrumentationResult result;
    result.stage_timings_ns.clear();
    result.stage_cycles.clear();
    result.symbol_memory_bytes.clear();
    result.stage_scratch_bytes.clear();

    host_sim::Scheduler scheduler;
    scheduler.configure({meta.sf, meta.bw, meta.sample_rate});

    if (use_fixed_numeric) {
        auto demod_stage = std::make_shared<host_sim::DemodStageQ15>();
        scheduler.attach_stage(demod_stage);
        result.stage_labels.emplace_back("Demod (Q15)");
    } else {
        auto demod_stage = std::make_shared<host_sim::DemodStage>();
        scheduler.attach_stage(demod_stage);
        result.stage_labels.emplace_back("Demod (float)");
    }

    host_sim::Scheduler::Hooks hooks;
    if (options.isr_latency_ns > 0.0) {
        const std::size_t last_stage = scheduler.stage_count() > 0 ? scheduler.stage_count() - 1 : 0;
        std::size_t completed_symbols = 0;
        hooks.after_stage = [last_stage, &options, &completed_symbols](std::size_t stage_index,
                                                                       host_sim::SymbolContext&,
                                                                       double& elapsed_ns) {
            if (stage_index != last_stage) {
                return;
            }
            ++completed_symbols;
            if (options.isr_every_symbols > 0) {
                if ((completed_symbols % options.isr_every_symbols) != 0) {
                    return;
                }
            }
            elapsed_ns += options.isr_latency_ns;
        };
    }

    std::optional<host_sim::RealTimeController> rt_controller;
    double effective_period_ns = 0.0;
    if (options.real_time_mode) {
        const double nominal_symbol_ns =
            (static_cast<double>(1ULL << meta.sf) / static_cast<double>(meta.bw)) * 1e9;
        const double speed = options.rt_speed > 0.0 ? options.rt_speed : 1.0;
        effective_period_ns = nominal_symbol_ns / speed;
        rt_controller.emplace(effective_period_ns, options.rt_tolerance_ns, options.rt_max_events);
        hooks = rt_controller->attach(scheduler, std::move(hooks));
        result.realtime_mode = true;
        result.realtime_symbol_period_ns = effective_period_ns;
    }

    scheduler.set_hooks(std::move(hooks));

    std::vector<double>* cycles_ptr = (cycle_scale > 0.0) ? &result.stage_cycles : nullptr;
    host_sim::Scheduler::Instrumentation instr{
        .stage_timings_ns = &result.stage_timings_ns,
        .stage_cycles = cycles_ptr,
        .symbol_memory_bytes = &result.symbol_memory_bytes,
        .stage_scratch_bytes = &result.stage_scratch_bytes,
        .cycle_scale_ns_to_cycles = cycle_scale,
    };
    scheduler.set_instrumentation(instr);

    const std::size_t samples_per_symbol = compute_samples_per_symbol(meta);
    const std::size_t available_symbols =
        (samples.size() > alignment_offset)
            ? (samples.size() - alignment_offset) / samples_per_symbol
            : 0;
    const std::size_t symbol_count = std::min<std::size_t>(available_symbols, max_symbols);

    FileSymbolSource source(samples, alignment_offset, samples_per_symbol, symbol_count);
    scheduler.run(source);
    result.stage_count = scheduler.stage_count();
    if (result.stage_count > 0 && !result.stage_timings_ns.empty()) {
        result.instrumented_symbols = result.stage_timings_ns.size() / result.stage_count;
    } else {
        result.instrumented_symbols = 0;
    }

    const double symbol_duration_ns =
        std::pow(2.0, static_cast<double>(meta.sf)) / static_cast<double>(meta.bw) * 1e9;
    result.symbol_duration_ns = symbol_duration_ns;

    if (result.instrumented_symbols > 0) {
        result.symbol_processing_ns.assign(result.instrumented_symbols, 0.0);
        for (std::size_t sym = 0; sym < result.instrumented_symbols; ++sym) {
            double sum = 0.0;
            for (std::size_t st = 0; st < result.stage_count; ++st) {
                const std::size_t idx = sym * result.stage_count + st;
                if (idx < result.stage_timings_ns.size()) {
                    sum += result.stage_timings_ns[idx];
                }
            }
            result.symbol_processing_ns[sym] = sum;
        }

        result.symbol_deadline_margins_ns.assign(result.instrumented_symbols, 0.0);
        double current_time_ns = 0.0;
        double dma_ready_time_ns = 0.0;
        double min_margin = std::numeric_limits<double>::max();
        std::size_t miss_count = 0;
        std::mt19937 rng(0);
        std::uniform_real_distribution<double> jitter_dist(-options.dma_jitter_ns,
                                                           options.dma_jitter_ns);

        const bool simulate_dma = options.enable_dma_sim && options.dma_fill_ns > 0.0;

        for (std::size_t sym = 0; sym < result.instrumented_symbols; ++sym) {
            const double processing_ns = result.symbol_processing_ns[sym];
            const double symbol_available_ns = static_cast<double>(sym) * symbol_duration_ns;
            double start_time_ns = std::max(current_time_ns, symbol_available_ns);
            if (simulate_dma) {
                start_time_ns = std::max(start_time_ns, dma_ready_time_ns);
            }

            const double finish_time_ns = start_time_ns + processing_ns;
            const double deadline_ns = symbol_available_ns + symbol_duration_ns;
            const double margin_ns = deadline_ns - finish_time_ns;
            result.symbol_deadline_margins_ns[sym] = margin_ns;
            if (margin_ns < min_margin) {
                min_margin = margin_ns;
            }
            if (margin_ns < 0.0) {
                ++miss_count;
            }

            current_time_ns = finish_time_ns;
            if (simulate_dma) {
                double jitter = (options.dma_jitter_ns > 0.0) ? jitter_dist(rng) : 0.0;
                double fill_time_ns = options.dma_fill_ns + jitter;
                if (fill_time_ns < 0.0) {
                    fill_time_ns = 0.0;
                }
                dma_ready_time_ns = start_time_ns + fill_time_ns;
            }
        }

        if (!result.symbol_deadline_margins_ns.empty()) {
            result.min_deadline_margin_ns = min_margin;
        }
        result.deadline_miss_count = miss_count;
    } else {
        result.min_deadline_margin_ns = 0.0;
        result.deadline_miss_count = 0;
    }

    if (rt_controller) {
        auto metrics = rt_controller->consume_metrics();
        if (!metrics.start_lag_ns.empty()) {
            const double sum =
                std::accumulate(metrics.start_lag_ns.begin(), metrics.start_lag_ns.end(), 0.0);
            result.rt_avg_start_lag_ns = sum / static_cast<double>(metrics.start_lag_ns.size());
            result.rt_max_start_lag_ns =
                *std::max_element(metrics.start_lag_ns.begin(), metrics.start_lag_ns.end());
        }
        if (!metrics.deadline_margin_ns.empty()) {
            const double sum = std::accumulate(metrics.deadline_margin_ns.begin(),
                                               metrics.deadline_margin_ns.end(),
                                               0.0);
            result.rt_avg_deadline_margin_ns =
                sum / static_cast<double>(metrics.deadline_margin_ns.size());
            result.rt_min_deadline_margin_ns =
                *std::min_element(metrics.deadline_margin_ns.begin(),
                                  metrics.deadline_margin_ns.end());
        } else if (result.realtime_mode) {
            result.rt_min_deadline_margin_ns = 0.0;
        }
        result.rt_overruns = std::move(metrics.overruns);
        result.rt_underruns = std::move(metrics.underruns);
    } else {
        result.rt_avg_start_lag_ns = 0.0;
        result.rt_max_start_lag_ns = 0.0;
        result.rt_avg_deadline_margin_ns = 0.0;
        result.rt_min_deadline_margin_ns = 0.0;
    }

    return result;
}

} // namespace host_sim::lora_replay
