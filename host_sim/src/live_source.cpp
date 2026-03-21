#include "host_sim/live_source.hpp"

#include <utility>

namespace host_sim
{

LiveSymbolSource::LiveSymbolSource(std::size_t max_buffered_symbols)
    : max_buffered_symbols_(max_buffered_symbols == 0 ? 1 : max_buffered_symbols)
{
}

void LiveSymbolSource::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = false;
    while (!queue_.empty()) {
        queue_.pop();
    }
}

std::optional<SymbolBuffer> LiveSymbolSource::next_symbol()
{
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] {
        return !queue_.empty() || closed_;
    });

    if (!queue_.empty()) {
        SymbolBuffer symbol = std::move(queue_.front());
        queue_.pop();
        cond_.notify_all();
        return symbol;
    }

    return std::nullopt;
}

bool LiveSymbolSource::push_symbol(SymbolBuffer symbol, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ensure_open_locked()) {
        return false;
    }

    if (!cond_.wait_for(lock, timeout, [this] {
            return queue_.size() < max_buffered_symbols_ || closed_;
        })) {
        return false;
    }

    if (!ensure_open_locked()) {
        return false;
    }

    queue_.push(std::move(symbol));
    cond_.notify_one();
    return true;
}

void LiveSymbolSource::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cond_.notify_all();
}

std::size_t LiveSymbolSource::queued_symbols() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool LiveSymbolSource::ensure_open_locked() const
{
    return !closed_;
}

} // namespace host_sim
