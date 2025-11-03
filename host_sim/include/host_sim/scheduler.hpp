#pragma once

#include "host_sim/stage.hpp"
#include "host_sim/symbol_source.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace host_sim
{

class Scheduler
{
public:
    struct Config
    {
        int sf{0};
        int bandwidth{0};
        int sample_rate{0};
    };

    struct Instrumentation
    {
        std::vector<double>* stage_timings_ns{nullptr};
        std::vector<std::size_t>* symbol_memory_bytes{nullptr};
    };

    void configure(const Config& config);

    void attach_stage(std::shared_ptr<Stage> stage);
    void clear_stages();

    void reset();
    void run(SymbolSource& source);

    const Config& config() const { return config_; }

    void set_instrumentation(Instrumentation instrumentation) { instrumentation_ = instrumentation; }

private:
    Config config_{};
    std::vector<std::shared_ptr<Stage>> stages_;
    std::size_t processed_symbols_{0};
    Instrumentation instrumentation_{};
};

} // namespace host_sim
