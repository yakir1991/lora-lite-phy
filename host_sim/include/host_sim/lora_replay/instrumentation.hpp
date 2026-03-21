#pragma once

#include "host_sim/lora_params.hpp"
#include "host_sim/lora_replay/options.hpp"
#include "host_sim/real_time_controller.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace host_sim::lora_replay
{

struct InstrumentationResult
{
    std::vector<double> stage_timings_ns;
    std::vector<double> stage_cycles;
    std::vector<std::size_t> symbol_memory_bytes;
    std::vector<std::size_t> stage_scratch_bytes;
    std::vector<std::string> stage_labels;
    std::size_t stage_count{0};
    std::size_t instrumented_symbols{0};
    std::vector<double> symbol_processing_ns;
    std::vector<double> symbol_deadline_margins_ns;
    double symbol_duration_ns{0.0};
    double min_deadline_margin_ns{0.0};
    std::size_t deadline_miss_count{0};
    bool realtime_mode{false};
    double realtime_symbol_period_ns{0.0};
    double rt_avg_start_lag_ns{0.0};
    double rt_max_start_lag_ns{0.0};
    double rt_avg_deadline_margin_ns{0.0};
    double rt_min_deadline_margin_ns{0.0};
    std::vector<host_sim::RealTimeController::Event> rt_overruns;
    std::vector<host_sim::RealTimeController::Event> rt_underruns;
};

std::size_t compute_samples_per_symbol(const host_sim::LoRaMetadata& meta);

InstrumentationResult run_scheduler_instrumentation(const std::vector<std::complex<float>>& samples,
                                                    const host_sim::LoRaMetadata& meta,
                                                    std::size_t alignment_offset,
                                                    std::size_t max_symbols,
                                                    double cycle_scale,
                                                    bool use_fixed_numeric,
                                                    const Options& options);

} // namespace host_sim::lora_replay
