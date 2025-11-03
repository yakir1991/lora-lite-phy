#include "host_sim/alignment.hpp"
#include "host_sim/capture.hpp"
#include "host_sim/lora_params.hpp"
#include "host_sim/scheduler.hpp"
#include "host_sim/stages/demod_stage.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace
{

class CollectorStage : public host_sim::Stage
{
public:
    void reset(const host_sim::StageConfig&) override { symbols_.clear(); }
    void process(host_sim::SymbolContext& ctx) override
    {
        if (ctx.has_demod_symbol) {
            symbols_.push_back(ctx.demod_symbol);
        }
    }
    void flush() override {}
    const std::vector<uint16_t>& symbols() const { return symbols_; }

private:
    std::vector<uint16_t> symbols_;
};

class FileSymbolSource : public host_sim::SymbolSource
{
public:
    FileSymbolSource(const std::vector<std::complex<float>>& samples,
                     std::size_t offset,
                     std::size_t samples_per_symbol,
                     std::size_t symbol_count)
        : samples_(samples), offset_(offset), sps_(samples_per_symbol), symbol_count_(symbol_count)
    {
    }

    void reset() override { index_ = 0; }

    std::optional<host_sim::SymbolBuffer> next_symbol() override
    {
        if (index_ >= symbol_count_) {
            return std::nullopt;
        }
        const auto start = offset_ + index_ * sps_;
        if (start + sps_ > samples_.size()) {
            return std::nullopt;
        }
        host_sim::SymbolBuffer buffer;
        buffer.samples.insert(buffer.samples.end(),
                               samples_.begin() + static_cast<std::ptrdiff_t>(start),
                               samples_.begin() + static_cast<std::ptrdiff_t>(start + sps_));
        ++index_;
        return buffer;
    }

private:
    const std::vector<std::complex<float>>& samples_;
    std::size_t offset_;
    std::size_t sps_;
    std::size_t symbol_count_;
    std::size_t index_{0};
};

std::size_t samples_per_symbol(const host_sim::LoRaMetadata& meta)
{
    const std::size_t chips = static_cast<std::size_t>(1) << meta.sf;
    return static_cast<std::size_t>((static_cast<long long>(meta.sample_rate) * chips) / meta.bw);
}

std::vector<std::string> load_capture_list(const fs::path& manifest_path)
{
    std::ifstream input(manifest_path);
    if (!input) {
        throw std::runtime_error("Failed to open manifest: " + manifest_path.string());
    }
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::string key = "\"capture\"";
    std::unordered_set<std::string> unique;
    std::vector<std::string> captures;

    std::size_t pos = 0;
    while ((pos = content.find(key, pos)) != std::string::npos) {
        auto colon = content.find(':', pos + key.size());
        if (colon == std::string::npos) {
            break;
        }
        auto first_quote = content.find('"', colon + 1);
        if (first_quote == std::string::npos) {
            break;
        }
        auto second_quote = content.find('"', first_quote + 1);
        if (second_quote == std::string::npos) {
            break;
        }
        std::string name = content.substr(first_quote + 1, second_quote - first_quote - 1);
        if (!name.empty() && unique.insert(name).second) {
            captures.push_back(std::move(name));
        }
        pos = second_quote + 1;
    }

    if (captures.empty()) {
        throw std::runtime_error("Manifest did not contain any capture entries");
    }

    return captures;
}

std::vector<uint16_t> run_symbols(const fs::path& capture_path,
                                  const host_sim::LoRaMetadata& meta,
                                  const std::vector<std::complex<float>>& samples,
                                  bool fixed_mode,
                                  std::size_t max_symbols)
{
    host_sim::FftDemodulator demod(meta.sf, meta.sample_rate, meta.bw);
    const std::size_t alignment_offset =
        host_sim::find_symbol_alignment(samples, demod, meta.preamble_len);
    const std::size_t sps = samples_per_symbol(meta);
    const std::size_t available_symbols =
        (samples.size() > alignment_offset) ? (samples.size() - alignment_offset) / sps : 0;
    if (available_symbols == 0) {
        throw std::runtime_error("No symbols available in capture: " + capture_path.string());
    }
    const std::size_t symbol_count = std::min<std::size_t>(available_symbols, max_symbols);

    FileSymbolSource source(samples, alignment_offset, sps, symbol_count);

    host_sim::Scheduler scheduler;
    scheduler.configure({meta.sf, meta.bw, meta.sample_rate});

    if (fixed_mode) {
        scheduler.attach_stage(std::make_shared<host_sim::DemodStageQ15>());
    } else {
        scheduler.attach_stage(std::make_shared<host_sim::DemodStage>());
    }
    auto collector = std::make_shared<CollectorStage>();
    scheduler.attach_stage(collector);

    scheduler.run(source);
    return collector->symbols();
}

struct NumericComparison
{
    std::size_t symbol_count{0};
    std::size_t symbol_mismatches{0};
    std::size_t bit_errors{0};
    int sf{0};
};

NumericComparison compare_modes(const fs::path& capture_path,
                                const fs::path& metadata_path,
                                bool verbose = false)
{
    const auto samples = host_sim::load_cf32(capture_path);
    const auto meta = host_sim::load_metadata(metadata_path);

    constexpr std::size_t kMaxSymbols = 256;
    const auto symbols_float = run_symbols(capture_path, meta, samples, false, kMaxSymbols);
    const auto symbols_fixed = run_symbols(capture_path, meta, samples, true, kMaxSymbols);

    if (symbols_float.size() != symbols_fixed.size()) {
        throw std::runtime_error("Symbol count mismatch between numeric modes on capture " +
                                 capture_path.string());
    }

    NumericComparison result{};
    result.symbol_count = symbols_float.size();
    result.sf = meta.sf;

    for (std::size_t idx = 0; idx < symbols_float.size(); ++idx) {
        const auto lhs = symbols_float[idx];
        const auto rhs = symbols_fixed[idx];
        if (lhs != rhs) {
            ++result.symbol_mismatches;
            const auto diff = static_cast<unsigned int>(lhs ^ rhs);
            result.bit_errors += std::popcount(diff & ((1u << static_cast<unsigned>(meta.sf)) - 1u));
            if (verbose) {
                std::cerr << "[numeric-modes] mismatch capture=" << capture_path.filename()
                          << " symbol=" << idx << " float=" << lhs << " q15=" << rhs << "\n";
            }
        }
    }

    return result;
}

} // namespace

int main()
{
    const fs::path root_dir = fs::path{PROJECT_SOURCE_DIR};
    const fs::path data_dir = root_dir / "gr_lora_sdr" / "data" / "generated";
    const fs::path manifest_path = root_dir / "docs" / "reference_stage_manifest.json";

    std::vector<std::string> capture_names;
    try {
        capture_names = load_capture_list(manifest_path);
    } catch (const std::exception& ex) {
        std::cerr << "Failed to load manifest: " << ex.what() << "\n";
        return 1;
    }

    bool verbose = (std::getenv("HOST_SIM_NUMERIC_VERBOSE") != nullptr);
    std::size_t total_symbols = 0;
    std::size_t total_mismatched = 0;
    std::size_t total_bit_errors = 0;
    std::size_t total_bits = 0;

    for (const auto& capture_name : capture_names) {
        const fs::path capture_path = data_dir / capture_name;
        fs::path metadata_file = capture_path;
        metadata_file.replace_extension(".json");

        if (!fs::exists(capture_path)) {
            std::cerr << "Capture missing: " << capture_path << "\n";
            return 1;
        }
        if (!fs::exists(metadata_file)) {
            std::cerr << "Metadata missing for capture: " << metadata_file << "\n";
            return 1;
        }

        NumericComparison comparison;
        try {
            comparison = compare_modes(capture_path, metadata_file, verbose);
        } catch (const std::exception& ex) {
            std::cerr << "Numeric comparison failed for " << capture_name << ": " << ex.what()
                      << "\n";
            return 1;
        }

        total_symbols += comparison.symbol_count;
        total_mismatched += comparison.symbol_mismatches;
        total_bit_errors += comparison.bit_errors;
        total_bits += comparison.symbol_count * static_cast<std::size_t>(comparison.sf);

        const double symbol_ratio = (comparison.symbol_count > 0)
            ? static_cast<double>(comparison.symbol_mismatches) /
                  static_cast<double>(comparison.symbol_count)
            : 0.0;
        const double bit_ratio = (comparison.symbol_count > 0 && comparison.sf > 0)
            ? static_cast<double>(comparison.bit_errors) /
                  (static_cast<double>(comparison.symbol_count) * static_cast<double>(comparison.sf))
            : 0.0;

        if (verbose || comparison.symbol_mismatches > 0) {
            std::cerr << "[numeric-modes] capture=" << capture_name
                      << " symbols=" << comparison.symbol_count
                      << " symbol_mismatches=" << comparison.symbol_mismatches
                      << " bit_errors=" << comparison.bit_errors
                      << " symbol_ratio=" << symbol_ratio
                      << " bit_ratio=" << bit_ratio << "\n";
        }

        constexpr double kSymbolMismatchTolerance = 0.01; // tolerate up to 1% symbol delta
        constexpr double kBitErrorTolerance = 0.02;       // tolerate up to 2% bit error rate
        if (symbol_ratio > kSymbolMismatchTolerance || bit_ratio > kBitErrorTolerance) {
            std::cerr << "Mismatch ratio too high: capture " << capture_name
                      << " symbol_ratio=" << symbol_ratio
                      << " bit_ratio=" << bit_ratio << "\n";
            return 1;
        }
    }

    if (verbose) {
        std::cerr << "[numeric-modes] aggregate symbols=" << total_symbols
                  << " symbol_mismatches=" << total_mismatched
                  << " bit_errors=" << total_bit_errors << "\n";
    }
    const double total_symbol_ratio = (total_symbols > 0)
        ? static_cast<double>(total_mismatched) / static_cast<double>(total_symbols)
        : 0.0;
    const double total_bit_ratio = (total_bits > 0)
        ? static_cast<double>(total_bit_errors) / static_cast<double>(total_bits)
        : 0.0;
    constexpr double kAggregateSymbolTolerance = 0.01;
    constexpr double kAggregateBitTolerance = 0.02;
    if (total_symbol_ratio > kAggregateSymbolTolerance || total_bit_ratio > kAggregateBitTolerance) {
        std::cerr << "Aggregate mismatch ratio too high: symbol_ratio=" << total_symbol_ratio
                  << " bit_ratio=" << total_bit_ratio << "\n";
        return 1;
    }

    return 0;
}
