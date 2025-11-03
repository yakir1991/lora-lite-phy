#include "host_sim/scheduler.hpp"

#include "host_sim/symbol_context.hpp"

#include <chrono>
#include <span>

namespace host_sim
{

void Scheduler::configure(const Config& config)
{
    config_ = config;
}

void Scheduler::attach_stage(std::shared_ptr<Stage> stage)
{
    stages_.push_back(std::move(stage));
}

void Scheduler::clear_stages()
{
    stages_.clear();
}

void Scheduler::reset()
{
    processed_symbols_ = 0;
    StageConfig stage_cfg{config_.sf, config_.bandwidth, config_.sample_rate};
    for (auto& stage : stages_) {
        stage->reset(stage_cfg);
    }
}

void Scheduler::run(SymbolSource& source)
{
    reset();
    source.reset();

    while (auto buffer = source.next_symbol()) {
        SymbolContext context{
            processed_symbols_,
            std::span<const std::complex<float>>(buffer->samples.data(), buffer->samples.size()),
        };
        for (auto& stage : stages_) {
            const auto start = std::chrono::steady_clock::now();
            stage->process(context);
            const auto end = std::chrono::steady_clock::now();
            context.stage_elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();
            if (instrumentation_.stage_timings_ns) {
                instrumentation_.stage_timings_ns->push_back(context.stage_elapsed_ns);
            }
        }
        if (instrumentation_.symbol_memory_bytes) {
            instrumentation_.symbol_memory_bytes->push_back(context.samples.size() * sizeof(std::complex<float>));
        }
        ++processed_symbols_;
    }

    for (auto& stage : stages_) {
        stage->flush();
    }
}

} // namespace host_sim
