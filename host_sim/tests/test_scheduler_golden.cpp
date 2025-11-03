#include "host_sim/alignment.hpp"
#include "host_sim/capture.hpp"
#include "host_sim/lora_params.hpp"
#include "host_sim/scheduler.hpp"
#include "host_sim/fft_demod_ref.hpp"
#include "host_sim/stages/demod_stage.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

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

    void reset() override
    {
        index_ = 0;
    }

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
        buffer.samples.insert(
            buffer.samples.end(),
            samples_.begin() + static_cast<std::ptrdiff_t>(start),
            samples_.begin() + static_cast<std::ptrdiff_t>(start + samples_per_symbol_));
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

class CollectorStage : public host_sim::Stage
{
public:
    void reset(const host_sim::StageConfig&) override
    {
        symbols_.clear();
    }

    void process(host_sim::SymbolContext& context) override
    {
        if (context.has_demod_symbol) {
            symbols_.push_back(context.demod_symbol);
        }
    }

    void flush() override {}

    const std::vector<uint16_t>& symbols() const { return symbols_; }

private:
    std::vector<uint16_t> symbols_;
};

std::size_t compute_samples_per_symbol(const host_sim::LoRaMetadata& meta)
{
    const std::size_t chips = static_cast<std::size_t>(1) << meta.sf;
    return static_cast<std::size_t>((static_cast<long long>(meta.sample_rate) * chips) / meta.bw);
}

} // namespace

int main()
{
    const fs::path root_dir = fs::path{PROJECT_SOURCE_DIR};
    const fs::path data_dir = root_dir / "gr_lora_sdr" / "data" / "generated";

    const fs::path capture_path = data_dir / "tx_rx_sf7_bw125000_cr1_snrm5p0.cf32";
    const fs::path metadata_path = data_dir / "tx_rx_sf7_bw125000_cr1_snrm5p0.json";

    auto samples = host_sim::load_cf32(capture_path);
    auto meta = host_sim::load_metadata(metadata_path);

    host_sim::FftDemodulator demod(meta.sf, meta.sample_rate, meta.bw);
    const std::size_t alignment_offset =
        host_sim::find_symbol_alignment(samples, demod, meta.preamble_len);
    const std::size_t samples_per_symbol = compute_samples_per_symbol(meta);
    const std::size_t available_symbols =
        (samples.size() > alignment_offset)
            ? (samples.size() - alignment_offset) / samples_per_symbol
            : 0;
    const std::size_t symbol_count = std::min<std::size_t>(available_symbols, 256);
    if (symbol_count == 0) {
        std::cerr << "No symbols available in capture\n";
        return 1;
    }

    FileSymbolSource source(samples, alignment_offset, samples_per_symbol, symbol_count);

    host_sim::Scheduler scheduler;
    scheduler.configure({meta.sf, meta.bw, meta.sample_rate});

    auto demod_stage = std::make_shared<host_sim::DemodStage>();
    auto collector = std::make_shared<CollectorStage>();

    scheduler.attach_stage(demod_stage);
    scheduler.attach_stage(collector);

    try {
        scheduler.run(source);
    } catch (const std::exception& ex) {
        std::cerr << "Scheduler run failed: " << ex.what() << "\n";
        return 1;
    }

    host_sim::FftDemodReference demod_reference(meta.sf, meta.sample_rate, meta.bw);
    std::vector<uint16_t> reference;
    reference.reserve(symbol_count);
    for (std::size_t i = 0; i < symbol_count; ++i) {
        const auto start = alignment_offset + i * samples_per_symbol;
        reference.push_back(
            demod_reference.demodulate(samples.data() + static_cast<std::ptrdiff_t>(start)));
    }

    const auto& symbols = collector->symbols();
    if (symbols.size() != reference.size()) {
        std::cerr << "Symbol count mismatch: got " << symbols.size()
                  << " expected " << reference.size() << "\n";
        return 1;
    }

    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (symbols[i] != reference[i]) {
            std::cerr << "Mismatch at symbol " << i << ": got " << symbols[i]
                      << " expected " << reference[i] << "\n";
            return 1;
        }
    }

    return 0;
}
