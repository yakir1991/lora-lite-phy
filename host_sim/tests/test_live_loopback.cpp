#include "host_sim/live_source.hpp"
#include "host_sim/scheduler.hpp"
#include "host_sim/stage.hpp"

#include <chrono>
#include <complex>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

namespace
{

struct ProcessRecord
{
    std::size_t symbol_index{0};
    std::complex<float> first_sample{};
};

class CaptureStage : public host_sim::Stage
{
public:
    void reset(const host_sim::StageConfig&) override
    {
        records_.clear();
    }

    void process(host_sim::SymbolContext& context) override
    {
        ProcessRecord record;
        record.symbol_index = context.symbol_index;
        if (!context.samples.empty()) {
            record.first_sample = context.samples.front();
        }
        records_.push_back(record);
    }

    void flush() override {}

    const std::vector<ProcessRecord>& records() const { return records_; }

private:
    std::vector<ProcessRecord> records_;
};

host_sim::SymbolBuffer make_buffer(std::size_t symbol_index, std::size_t samples_per_symbol = 4)
{
    host_sim::SymbolBuffer buffer;
    buffer.samples.reserve(samples_per_symbol);
    for (std::size_t i = 0; i < samples_per_symbol; ++i) {
        buffer.samples.emplace_back(static_cast<float>(symbol_index),
                                    static_cast<float>(i));
    }
    return buffer;
}

} // namespace

int main()
{
    host_sim::LiveSymbolSource source(4);

    host_sim::Scheduler scheduler;
    scheduler.configure({7, 125000, 500000});
    auto stage = std::make_shared<CaptureStage>();
    scheduler.attach_stage(stage);

    std::thread producer([&source]() {
        for (std::size_t symbol = 0; symbol < 6; ++symbol) {
            auto buffer = make_buffer(symbol);
            if (!source.push_symbol(std::move(buffer), std::chrono::milliseconds(500))) {
                source.close();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        source.close();
    });

    scheduler.run(source);
    producer.join();

    const auto& records = stage->records();
    if (records.size() != 6) {
        return 1;
    }
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (records[i].symbol_index != i) {
            return 1;
        }
        if (records[i].first_sample.real() != static_cast<float>(i)) {
            return 1;
        }
    }

    return 0;
}
