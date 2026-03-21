#pragma once

#include "host_sim/symbol_source.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace host_sim
{

class LiveSymbolSource : public SymbolSource
{
public:
    explicit LiveSymbolSource(std::size_t max_buffered_symbols = 32);

    void reset() override;
    std::optional<SymbolBuffer> next_symbol() override;

    bool push_symbol(SymbolBuffer symbol,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

    void close();

    std::size_t queued_symbols() const;

private:
    bool ensure_open_locked() const;

    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::queue<SymbolBuffer> queue_;
    std::size_t max_buffered_symbols_;
    bool closed_{false};
};

} // namespace host_sim
