#include "host_sim/scheduler.hpp"
#include "host_sim/symbol_context.hpp"
#include "host_sim/symbol_source.hpp"

#include <complex>
#include <cstddef>
#include <optional>
#include <vector>

namespace
{

class VectorSymbolSource : public host_sim::SymbolSource
{
public:
    explicit VectorSymbolSource(std::size_t symbol_count, std::size_t samples_per_symbol = 4)
    {
        buffers_.reserve(symbol_count);
        for (std::size_t i = 0; i < symbol_count; ++i) {
            host_sim::SymbolBuffer buffer;
            buffer.samples.resize(samples_per_symbol);
            for (std::size_t j = 0; j < samples_per_symbol; ++j) {
                buffer.samples[j] = std::complex<float>(static_cast<float>(i), static_cast<float>(j));
            }
            buffers_.push_back(std::move(buffer));
        }
    }

    void reset() override { index_ = 0; }

    std::optional<host_sim::SymbolBuffer> next_symbol() override
    {
        if (index_ >= buffers_.size()) {
            return std::nullopt;
        }
        return buffers_[index_++];
    }

private:
    std::vector<host_sim::SymbolBuffer> buffers_;
    std::size_t index_{0};
};

struct ProcessRecord
{
    std::size_t symbol_index{0};
    int sf{0};
};

class SpyStage : public host_sim::Stage
{
public:
    void reset(const host_sim::StageConfig& config) override
    {
        current_config_ = config;
        reset_history_.push_back(config);
        run_offsets_.push_back(process_records_.size());
    }

    void process(host_sim::SymbolContext& context) override
    {
        process_records_.push_back(ProcessRecord{context.symbol_index, current_config_.sf});
    }

    void flush() override {}

    const std::vector<host_sim::StageConfig>& resets() const { return reset_history_; }
    const std::vector<std::size_t>& run_offsets() const { return run_offsets_; }
    const std::vector<ProcessRecord>& records() const { return process_records_; }

private:
    host_sim::StageConfig current_config_{};
    std::vector<host_sim::StageConfig> reset_history_;
    std::vector<std::size_t> run_offsets_;
    std::vector<ProcessRecord> process_records_;
};

bool verify_sequence(const std::vector<ProcessRecord>& records,
                     std::size_t begin,
                     std::size_t end,
                     int expected_sf)
{
    if (end > records.size()) {
        return false;
    }
    for (std::size_t i = begin; i < end; ++i) {
        const auto& rec = records[i];
        if (rec.symbol_index != (i - begin)) {
            return false;
        }
        if (rec.sf != expected_sf) {
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    host_sim::Scheduler scheduler;
    auto spy = std::make_shared<SpyStage>();
    scheduler.attach_stage(spy);

    scheduler.configure({7, 125000, 500000});
    VectorSymbolSource first_run_source(3);
    scheduler.run(first_run_source);

    scheduler.configure({9, 250000, 1000000});
    VectorSymbolSource second_run_source(2);
    scheduler.run(second_run_source);

    const auto& resets = spy->resets();
    const auto& offsets = spy->run_offsets();
    const auto& records = spy->records();

    if (resets.size() != 2) {
        return 1;
    }
    if (resets[0].sf != 7 || resets[1].sf != 9) {
        return 1;
    }
    if (offsets.size() != 2 || offsets[0] != 0 || offsets[1] != 3) {
        return 1;
    }
    if (records.size() != 5) {
        return 1;
    }
    if (!verify_sequence(records, 0, 3, 7)) {
        return 1;
    }
    if (!verify_sequence(records, 3, 5, 9)) {
        return 1;
    }

    return 0;
}
