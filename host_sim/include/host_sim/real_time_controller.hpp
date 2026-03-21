#pragma once

#include "host_sim/scheduler.hpp"
#include "host_sim/symbol_context.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace host_sim
{

class RealTimeController
{
public:
    struct Event
    {
        std::size_t symbol_index{0};
        double timestamp_ns{0.0};
        double delta_ns{0.0};
    };

    struct Metrics
    {
        std::vector<double> start_lag_ns;
        std::vector<double> deadline_margin_ns;
        std::vector<Event> overruns;
        std::vector<Event> underruns;
        double max_start_lag_ns{0.0};
        double min_deadline_margin_ns{0.0};
    };

    RealTimeController(double symbol_period_ns,
                       double tolerance_ns,
                       std::size_t max_event_records);

    Scheduler::Hooks attach(Scheduler& scheduler, Scheduler::Hooks hooks);
    Metrics consume_metrics();

private:
    void handle_before_stage(std::size_t stage_index, SymbolContext& context);
    void handle_after_stage(std::size_t stage_index, SymbolContext& context);

    void record_overrun(std::size_t symbol_index,
                        double lag_ns,
                        double timestamp_ns);
    void record_underrun(std::size_t symbol_index,
                         double margin_ns,
                         double timestamp_ns);

    const double period_ns_;
    const double tolerance_ns_;
    const std::size_t max_events_;

    bool started_{false};
    std::chrono::steady_clock::time_point start_time_{};
    std::size_t last_stage_index_{0};

    mutable std::mutex mutex_;
    Metrics metrics_;
};

} // namespace host_sim
