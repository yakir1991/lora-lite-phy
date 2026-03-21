#include "host_sim/real_time_controller.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace host_sim
{

RealTimeController::RealTimeController(double symbol_period_ns,
                                       double tolerance_ns,
                                       std::size_t max_event_records)
    : period_ns_(symbol_period_ns),
      tolerance_ns_(tolerance_ns < 0.0 ? 0.0 : tolerance_ns),
      max_events_(max_event_records == 0 ? 0 : max_event_records)
{
    metrics_.min_deadline_margin_ns = std::numeric_limits<double>::max();
}

Scheduler::Hooks RealTimeController::attach(Scheduler& scheduler, Scheduler::Hooks hooks)
{
    last_stage_index_ = scheduler.stage_count() > 0 ? scheduler.stage_count() - 1 : 0;

    auto previous_before = hooks.before_stage;
    hooks.before_stage = [this, previous_before](std::size_t stage_index, SymbolContext& context) {
        handle_before_stage(stage_index, context);
        if (previous_before) {
            previous_before(stage_index, context);
        }
    };

    auto previous_after = hooks.after_stage;
    hooks.after_stage = [this, previous_after](std::size_t stage_index, SymbolContext& context, double& elapsed) {
        if (previous_after) {
            previous_after(stage_index, context, elapsed);
        }
        handle_after_stage(stage_index, context);
    };

    return hooks;
}

void RealTimeController::handle_before_stage(std::size_t stage_index, SymbolContext& context)
{
    if (stage_index != 0) {
        return;
    }

    auto now_initial = std::chrono::steady_clock::now();
    if (!started_) {
        started_ = true;
        start_time_ = now_initial;
    }

    const auto symbol_index = context.symbol_index;
    const auto offset_ns = static_cast<long long>(std::llround(symbol_index * period_ns_));
    const auto expected_start = start_time_ + std::chrono::nanoseconds(offset_ns);

    auto now = now_initial;
    if (now < expected_start) {
        std::this_thread::sleep_until(expected_start);
        now = expected_start;
    }

    const double lag_ns =
        std::chrono::duration<double, std::nano>(now - expected_start).count();
    const double timestamp_ns =
        std::chrono::duration<double, std::nano>(now - start_time_).count();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.start_lag_ns.push_back(lag_ns);
        if (lag_ns > metrics_.max_start_lag_ns) {
            metrics_.max_start_lag_ns = lag_ns;
        }
    }

    if (lag_ns > tolerance_ns_) {
        record_overrun(symbol_index, lag_ns, timestamp_ns);
    }
}

void RealTimeController::handle_after_stage(std::size_t stage_index, SymbolContext& context)
{
    if (stage_index != last_stage_index_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto symbol_index = context.symbol_index;
    const auto offset_ns =
        static_cast<long long>(std::llround((symbol_index + 1) * period_ns_));
    const auto expected_finish = start_time_ + std::chrono::nanoseconds(offset_ns);
    const double margin_ns =
        std::chrono::duration<double, std::nano>(expected_finish - now).count();
    const double timestamp_ns =
        std::chrono::duration<double, std::nano>(now - start_time_).count();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_.deadline_margin_ns.push_back(margin_ns);
        if (margin_ns < metrics_.min_deadline_margin_ns) {
            metrics_.min_deadline_margin_ns = margin_ns;
        }
    }

    if (margin_ns < -tolerance_ns_) {
        record_underrun(symbol_index, margin_ns, timestamp_ns);
    }
}

RealTimeController::Metrics RealTimeController::consume_metrics()
{
    std::lock_guard<std::mutex> lock(mutex_);
    Metrics out = std::move(metrics_);
    metrics_ = Metrics{};
    metrics_.min_deadline_margin_ns = std::numeric_limits<double>::max();
    return out;
}

void RealTimeController::record_overrun(std::size_t symbol_index,
                                        double lag_ns,
                                        double timestamp_ns)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (max_events_ == 0 || metrics_.overruns.size() < max_events_) {
        metrics_.overruns.push_back(Event{symbol_index, timestamp_ns, lag_ns});
    }
}

void RealTimeController::record_underrun(std::size_t symbol_index,
                                         double margin_ns,
                                         double timestamp_ns)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (max_events_ == 0 || metrics_.underruns.size() < max_events_) {
        metrics_.underruns.push_back(Event{symbol_index, timestamp_ns, margin_ns});
    }
}

} // namespace host_sim
