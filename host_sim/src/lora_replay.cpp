#include "host_sim/alignment.hpp"
#include "host_sim/capture.hpp"
#include "host_sim/deinterleaver.hpp"
#include "host_sim/fft_demod.hpp"
#include "host_sim/fft_demod_ref.hpp"
#include "host_sim/hamming.hpp"
#include "host_sim/lora_params.hpp"
#include "host_sim/soft_decode.hpp"
#include "host_sim/scheduler.hpp"
#include "host_sim/stages/demod_stage.hpp"
#include "host_sim/whitening.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

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

    void reset() override { index_ = 0; }

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

uint16_t compute_lora_crc(const std::vector<uint8_t>& payload)
{
    uint16_t crc = 0x0000;
    for (uint8_t byte : payload) {
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

std::size_t compute_samples_per_symbol(const host_sim::LoRaMetadata& meta)
{
    const std::size_t chips = static_cast<std::size_t>(1) << meta.sf;
    return static_cast<std::size_t>((static_cast<long long>(meta.sample_rate) * chips) / meta.bw);
}

struct InstrumentationResult
{
    std::vector<double> stage_timings_ns;
    std::vector<std::size_t> symbol_memory_bytes;
};

InstrumentationResult run_scheduler_instrumentation(const std::vector<std::complex<float>>& samples,
                                                    const host_sim::LoRaMetadata& meta,
                                                    std::size_t alignment_offset,
                                                    std::size_t max_symbols)
{
    InstrumentationResult result;
    host_sim::Scheduler scheduler;
    scheduler.configure({meta.sf, meta.bw, meta.sample_rate});

    auto demod_stage = std::make_shared<host_sim::DemodStage>();
    scheduler.attach_stage(demod_stage);

    scheduler.set_instrumentation({&result.stage_timings_ns, &result.symbol_memory_bytes});

    const std::size_t samples_per_symbol = compute_samples_per_symbol(meta);
    const std::size_t available_symbols =
        (samples.size() > alignment_offset)
            ? (samples.size() - alignment_offset) / samples_per_symbol
            : 0;
    const std::size_t symbol_count = std::min<std::size_t>(available_symbols, max_symbols);

    FileSymbolSource source(samples, alignment_offset, samples_per_symbol, symbol_count);
    scheduler.run(source);

    return result;
}

struct Options
{
    std::filesystem::path iq_file;
    std::string payload;
    std::optional<std::filesystem::path> stats_output;
    std::optional<std::filesystem::path> metadata;
    std::optional<std::filesystem::path> dump_symbols;
    std::optional<std::filesystem::path> dump_iq;
    std::optional<std::filesystem::path> compare_root;
    std::optional<std::filesystem::path> dump_stages;
    std::optional<std::filesystem::path> dump_payload;
    std::optional<std::filesystem::path> summary_output;
    bool multi_packet{false};
    bool soft{false};
};

struct HeaderDecodeResult
{
    bool success{false};
    int payload_len{0};
    bool has_crc{false};
    int cr{0};
    int checksum_field{0};
    int checksum_computed{0};
    std::size_t consumed_symbols{0};
    std::vector<uint16_t> codewords;
    std::vector<uint8_t> nibbles;
};

struct StageOutputs
{
    std::vector<uint16_t> fft;
    std::vector<uint16_t> gray;
    std::vector<uint16_t> deinterleaver;
    std::vector<uint8_t> hamming;
};

struct StageComparisonResult
{
    std::string label;
    std::size_t host_count{0};
    std::size_t ref_count{0};
    std::size_t mismatches{0};
    std::optional<std::size_t> alignment_offset;
    bool alignment_relative_to_reference{true};
    std::optional<std::size_t> first_diff_index;
    std::optional<long long> host_value;
    std::optional<long long> ref_value;
    bool reference_missing{false};
};

struct SummaryReport
{
    std::optional<std::string> capture_path;
    std::optional<host_sim::LoRaMetadata> metadata;
    std::optional<host_sim::CaptureStats> stats;
    std::vector<uint16_t> preview_symbols;
    std::vector<StageComparisonResult> stage_results;
    std::size_t reference_mismatches{0};
    std::size_t stage_mismatches{0};
    bool whitening_roundtrip_ok{true};
    bool compare_run{false};
    std::vector<double> stage_timings_ns;
    std::vector<std::size_t> memory_usage_bytes;
};

void print_usage(const char* binary)
{
    std::cerr << "Usage: " << binary << " --iq <capture.cf32>"
              << " [--metadata <file.json>]"
              << " [--payload <ascii>]"
              << " [--stats <file.json>]"
              << " [--dump-symbols <file.txt>]"
              << " [--dump-iq <file.cf32>]"
              << " [--compare-root <path/prefix>]"
              << " [--dump-stages <path/prefix>]"
              << " [--dump-payload <file.bin>]"
              << " [--summary <file.json>]"
              << " [--multi]"
              << "\n";
}

Options parse_arguments(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--iq" && i + 1 < argc) {
            opts.iq_file = argv[++i];
        } else if (arg == "--payload" && i + 1 < argc) {
            opts.payload = argv[++i];
        } else if (arg == "--metadata" && i + 1 < argc) {
            opts.metadata = std::filesystem::path{argv[++i]};
        } else if (arg == "--stats" && i + 1 < argc) {
            opts.stats_output = std::filesystem::path{argv[++i]};
        } else if (arg == "--dump-symbols" && i + 1 < argc) {
            opts.dump_symbols = std::filesystem::path{argv[++i]};
        } else if (arg == "--dump-iq" && i + 1 < argc) {
            opts.dump_iq = std::filesystem::path{argv[++i]};
        } else if (arg == "--compare-root" && i + 1 < argc) {
            opts.compare_root = std::filesystem::path{argv[++i]};
        } else if (arg == "--dump-stages" && i + 1 < argc) {
            opts.dump_stages = std::filesystem::path{argv[++i]};
        } else if (arg == "--dump-payload" && i + 1 < argc) {
            opts.dump_payload = std::filesystem::path{argv[++i]};
        } else if (arg == "--summary" && i + 1 < argc) {
            opts.summary_output = std::filesystem::path{argv[++i]};
        } else if (arg == "--multi") {
            opts.multi_packet = true;
        } else if (arg == "--soft") {
            opts.soft = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::runtime_error("Unknown or incomplete argument: " + std::string(arg));
        }
    }
    if (opts.iq_file.empty()) {
        throw std::runtime_error("Missing required --iq argument");
    }
    return opts;
}

void write_stats_json(const std::filesystem::path& path,
                      const host_sim::CaptureStats& stats,
                      const Options& opts)
{
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Unable to open stats output file: " + path.string());
    }

    out << "{\n"
        << "  \"iq_file\": \"" << opts.iq_file.generic_string() << "\",\n"
        << "  \"sample_count\": " << stats.sample_count << ",\n"
        << "  \"min_magnitude\": " << std::setprecision(6) << stats.min_magnitude << ",\n"
        << "  \"max_magnitude\": " << std::setprecision(6) << stats.max_magnitude << ",\n"
        << "  \"mean_power\": " << std::setprecision(6) << stats.mean_power << "\n"
        << "}\n";
}

HeaderDecodeResult try_decode_header(const std::vector<uint16_t>& symbols,
                                     std::size_t start,
                                     const host_sim::LoRaMetadata& meta)
{
    const bool debug_header = (std::getenv("HOST_SIM_DEBUG_HEADER") != nullptr);
    HeaderDecodeResult result;
    if (start + 8 > symbols.size()) {
        return result;
    }

    host_sim::DeinterleaverConfig header_cfg{meta.sf, 4, true, meta.ldro};
    const int block_symbols = 8;
    std::size_t cursor = start;
    std::size_t total_consumed = 0;
    std::vector<uint8_t> header_nibbles;
    std::vector<uint16_t> header_codewords;
    while (header_nibbles.size() < 5 && cursor + block_symbols <= symbols.size()) {
        std::vector<uint16_t> header_input(symbols.begin() + cursor,
                                           symbols.begin() + cursor + block_symbols);
        if (debug_header) {
            std::cout << "Header symbols (start=" << cursor << "):";
            for (auto value : header_input) {
                std::cout << ' ' << value;
            }
            std::cout << "\n";
        }

        std::size_t consumed_block = 0;
        auto codewords = host_sim::deinterleave(header_input, header_cfg, consumed_block);
        if (consumed_block == 0) {
            break;
        }
        total_consumed += consumed_block;
        cursor += consumed_block;
        header_codewords.insert(header_codewords.end(), codewords.begin(), codewords.end());

        if (debug_header) {
            std::cout << "Deinterleaved codewords:";
            for (auto cw : codewords) {
                std::cout << ' ' << std::hex << static_cast<int>(cw) << std::dec;
            }
            std::cout << "\n";
        }

        auto nibbles = host_sim::hamming_decode_block(codewords, true, 4);
        if (debug_header) {
            std::cout << "Header nibbles:";
            for (auto nib : nibbles) {
                std::cout << ' ' << std::hex << static_cast<int>(nib & 0xF) << std::dec;
            }
            std::cout << "\n";
        }
        header_nibbles.insert(header_nibbles.end(), nibbles.begin(), nibbles.end());
    }

    if (header_nibbles.size() < 5) {
        result.codewords = std::move(header_codewords);
        result.nibbles = header_nibbles;
        result.consumed_symbols = total_consumed;
        return result;
    }

    const int n0 = header_nibbles[0] & 0xF;
    const int n1 = header_nibbles[1] & 0xF;
    const int n2 = header_nibbles[2] & 0xF;
    const int n3 = header_nibbles[3] & 0xF;
    const int n4 = header_nibbles[4] & 0xF;

    const int payload_len = (n0 << 4) | n1;
    const bool has_crc = (n2 & 0x1) != 0;
    const int cr = (n2 >> 1) & 0x7;
    const int header_chk = ((n3 & 0x1) << 4) | n4;

    const bool c4 = ((n0 & 0x8) >> 3) ^ ((n0 & 0x4) >> 2) ^ ((n0 & 0x2) >> 1) ^ (n0 & 0x1);
    const bool c3 = ((n0 & 0x8) >> 3) ^ ((n1 & 0x8) >> 3) ^ ((n1 & 0x4) >> 2) ^ ((n1 & 0x2) >> 1) ^ (n2 & 0x1);
    const bool c2 = ((n0 & 0x4) >> 2) ^ ((n1 & 0x8) >> 3) ^ (n1 & 0x1) ^ ((n2 & 0x8) >> 3) ^ ((n2 & 0x2) >> 1);
    const bool c1 = ((n0 & 0x2) >> 1) ^ ((n1 & 0x4) >> 2) ^ (n1 & 0x1) ^ ((n2 & 0x4) >> 2) ^ ((n2 & 0x2) >> 1) ^ (n2 & 0x1);
    const bool c0 = (n0 & 0x1) ^ ((n1 & 0x2) >> 1) ^ ((n2 & 0x8) >> 3) ^ ((n2 & 0x4) >> 2) ^ ((n2 & 0x2) >> 1) ^ (n2 & 0x1);
    const int computed_checksum = (static_cast<int>(c4) << 4) | (static_cast<int>(c3) << 3) |
                                  (static_cast<int>(c2) << 2) | (static_cast<int>(c1) << 1) |
                                  static_cast<int>(c0);

    result.checksum_field = header_chk;
    result.checksum_computed = computed_checksum;

    if (payload_len <= 0 || header_chk != computed_checksum) {
        result.nibbles = header_nibbles;
        result.payload_len = payload_len;
        result.has_crc = has_crc;
        result.cr = cr;
        return result;
    }

    result.success = true;
    result.payload_len = payload_len;
    result.has_crc = has_crc;
    result.cr = cr;
    result.checksum_field = header_chk;
    result.checksum_computed = computed_checksum;
    result.consumed_symbols = total_consumed;
    result.codewords = std::move(header_codewords);
    result.nibbles = std::move(header_nibbles);
    return result;
}

// Upsample complex IQ data by 2x using linear interpolation.
// Doubles the effective sample rate so the demodulator gets finer
// timing resolution, extending SFO tolerance for long payloads.
std::vector<std::complex<float>> upsample_2x(
    const std::complex<float>* data, std::size_t count)
{
    if (count < 2) {
        return count == 1
            ? std::vector<std::complex<float>>{data[0]}
            : std::vector<std::complex<float>>{};
    }
    std::vector<std::complex<float>> out(count * 2 - 1);
    for (std::size_t i = 0; i < count - 1; ++i) {
        out[2 * i] = data[i];
        out[2 * i + 1] = 0.5f * (data[i] + data[i + 1]);
    }
    out[2 * (count - 1)] = data[count - 1];
    return out;
}

// Quick CRC probe: decode payload from symbols and check CRC.
// Used for data-start timing refinement at low oversampling,
// where SFO-induced timing drift can shift data symbols by ±1 bin.
bool probe_payload_crc(const std::vector<uint16_t>& symbols,
                       const HeaderDecodeResult& hdr,
                       const host_sim::LoRaMetadata& meta)
{
    if (!hdr.success) return false;
    const int pl = hdr.payload_len > 0 ? hdr.payload_len : meta.payload_len;
    const int cr = hdr.cr > 0 ? hdr.cr : meta.cr;
    const bool has_crc = hdr.has_crc || meta.has_crc;
    if (!has_crc || pl < 3) return false;

    std::vector<uint8_t> payload_nibbles;
    if (hdr.nibbles.size() > 5) {
        for (std::size_t i = 5; i < hdr.nibbles.size(); ++i) {
            payload_nibbles.push_back(hdr.nibbles[i]);
        }
    }

    const std::size_t nibble_target =
        static_cast<std::size_t>(pl) * 2 + 4;
    const int cw_len = cr + 4;
    std::size_t cursor = hdr.consumed_symbols > 0 ? hdr.consumed_symbols : 8;
    host_sim::DeinterleaverConfig payload_cfg{meta.sf, cr, false, meta.ldro};

    while (cursor + static_cast<std::size_t>(cw_len) <= symbols.size() &&
           payload_nibbles.size() < nibble_target) {
        std::vector<uint16_t> block(symbols.begin() + static_cast<std::ptrdiff_t>(cursor),
                                    symbols.begin() + static_cast<std::ptrdiff_t>(cursor + cw_len));
        std::size_t consumed = 0;
        auto codewords = host_sim::deinterleave(block, payload_cfg, consumed);
        if (consumed == 0) break;
        cursor += consumed;
        auto nibs = host_sim::hamming_decode_block(codewords, false, cr);
        payload_nibbles.insert(payload_nibbles.end(), nibs.begin(), nibs.end());
    }

    if (payload_nibbles.size() < static_cast<std::size_t>(pl) * 2 + 4)
        return false;

    host_sim::WhiteningSequencer seq;
    auto whitening = seq.sequence(payload_nibbles.size() / 2);

    std::vector<uint8_t> unwhitened;
    for (std::size_t i = 0; i + 1 < payload_nibbles.size() &&
         unwhitened.size() < static_cast<std::size_t>(pl) + 2; i += 2) {
        const std::size_t byte_idx = i / 2;
        uint8_t lo, hi;
        if (byte_idx < static_cast<std::size_t>(pl)) {
            const uint8_t w = (byte_idx < whitening.size()) ? whitening[byte_idx] : 0;
            lo = (payload_nibbles[i] & 0xF) ^ (w & 0x0F);
            hi = (payload_nibbles[i + 1] & 0xF) ^ ((w >> 4) & 0x0F);
        } else {
            lo = payload_nibbles[i] & 0xF;
            hi = payload_nibbles[i + 1] & 0xF;
        }
        unwhitened.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }

    if (unwhitened.size() < static_cast<std::size_t>(pl) + 2)
        return false;

    std::vector<uint8_t> crc_data(unwhitened.begin(),
                                  unwhitened.begin() + pl - 2);
    uint16_t computed = compute_lora_crc(crc_data);
    if (pl >= 2) {
        computed ^= unwhitened[pl - 1];
        computed ^= static_cast<uint16_t>(unwhitened[pl - 2]) << 8;
    }
    const uint16_t decoded = static_cast<uint16_t>(unwhitened[pl]) |
                             (static_cast<uint16_t>(unwhitened[pl + 1]) << 8);
    return computed == decoded;
}

uint16_t normalize_fft_symbol(uint16_t symbol, bool reduce_by_four, int sf)
{
    const uint16_t mask_full = static_cast<uint16_t>((1u << sf) - 1u);
    uint16_t adjusted = static_cast<uint16_t>((symbol - 1u) & mask_full);
    if (reduce_by_four) {
        adjusted = static_cast<uint16_t>(adjusted >> 2);
    }
    return adjusted;
}

void append_fft_gray(const std::vector<uint16_t>& block_symbols,
                     bool is_header_block,
                     bool ldro_enabled,
                     int sf,
                     StageOutputs& outputs)
{
    for (uint16_t symbol : block_symbols) {
        const bool reduce = is_header_block || ldro_enabled;
        const uint16_t fft_val = normalize_fft_symbol(symbol, reduce, sf);
        outputs.fft.push_back(fft_val);
        outputs.gray.push_back(static_cast<uint16_t>(fft_val ^ (fft_val >> 1u)));
    }
}

std::vector<long long> read_stage_file(const std::filesystem::path& path)
{
    std::vector<long long> values;
    std::ifstream input(path);
    if (!input) {
        return values;
    }
    long long value = 0;
    while (input >> value) {
        values.push_back(value);
    }
    return values;
}

template <typename Value>
void write_stage_file(const std::filesystem::path& path, const std::vector<Value>& values)
{
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Failed to write stage dump: " + path.string());
    }
    for (auto value : values) {
        out << static_cast<long long>(value) << '\n';
    }
}

template <typename HostType>
StageComparisonResult compare_stage(const std::string& label,
                                    const std::vector<HostType>& host,
                                    const std::vector<long long>& reference)
{
    StageComparisonResult result;
    result.label = label;
    result.host_count = host.size();
    result.ref_count = reference.size();

    if (host.empty()) {
        if (!reference.empty()) {
            result.mismatches = reference.size();
            result.alignment_offset = 0;
            result.first_diff_index = 0;
            result.ref_value = reference.front();
        }
        return result;
    }

    if (reference.empty()) {
        result.mismatches = result.host_count;
        result.alignment_relative_to_reference = false;
        result.alignment_offset = 0;
        result.first_diff_index = 0;
        result.host_value = static_cast<long long>(host.front());
        return result;
    }

    const bool host_is_longer = result.host_count >= result.ref_count;
    const std::size_t compare_len = host_is_longer ? result.ref_count : result.host_count;
    if (compare_len == 0) {
        return result;
    }

    const std::size_t max_offset =
        host_is_longer ? (result.host_count - compare_len) : (result.ref_count - compare_len);

    auto evaluate_alignment = [&](std::size_t host_start, std::size_t ref_start) {
        std::size_t mismatches = 0;
        std::optional<std::size_t> first_diff;
        std::optional<long long> first_host;
        std::optional<long long> first_ref;
        for (std::size_t idx = 0; idx < compare_len; ++idx) {
            const long long host_val = static_cast<long long>(host[host_start + idx]);
            const long long ref_val = reference[ref_start + idx];
            if (host_val != ref_val) {
                ++mismatches;
                if (!first_diff) {
                    first_diff = idx;
                    first_host = host_val;
                    first_ref = ref_val;
                }
            }
        }
        return std::tuple<std::size_t, std::optional<std::size_t>, std::optional<long long>, std::optional<long long>>(
            mismatches, first_diff, first_host, first_ref);
    };

    std::size_t best_offset = 0;
    std::size_t best_mismatches = std::numeric_limits<std::size_t>::max();
    std::optional<std::size_t> best_first_diff;
    std::optional<long long> best_host_value;
    std::optional<long long> best_ref_value;

    for (std::size_t offset = 0; offset <= max_offset; ++offset) {
        const std::size_t host_start = host_is_longer ? offset : 0;
        const std::size_t ref_start = host_is_longer ? 0 : offset;
        auto [mism, first_diff, host_val, ref_val] = evaluate_alignment(host_start, ref_start);
        if (mism < best_mismatches) {
            best_mismatches = mism;
            best_offset = offset;
            best_first_diff = first_diff;
            best_host_value = host_val;
            best_ref_value = ref_val;
            if (best_mismatches == 0) {
                break;
            }
        }
    }

    result.mismatches = (best_mismatches == std::numeric_limits<std::size_t>::max())
                            ? compare_len
                            : best_mismatches;
    result.alignment_offset = best_offset;
    result.alignment_relative_to_reference = !host_is_longer;
    if (best_first_diff) {
        result.first_diff_index = best_first_diff;
        result.host_value = best_host_value;
        result.ref_value = best_ref_value;
    }

    return result;
}

std::string build_stage_summary_token(const StageComparisonResult& result)
{
    std::ostringstream token;
    if (result.reference_missing) {
        token << "missing";
        return token.str();
    }

    if (result.mismatches == 0) {
        token << "OK";
        return token.str();
    }

    token << "FAIL(" << result.mismatches;
    if (result.host_count > 0) {
        token << '/' << result.host_count;
    }
    if (result.first_diff_index) {
        token << "@idx" << *result.first_diff_index;
    }
    token << ')';
    return token.str();
}

inline const char* json_bool(bool value)
{
    return value ? "true" : "false";
}

std::string json_escape(const std::string& value)
{
    std::string result;
    result.reserve(value.size() + 4);
    const char* hex = "0123456789abcdef";
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (ch < 0x20) {
                result += "\\u00";
                result += hex[(ch >> 4) & 0xF];
                result += hex[ch & 0xF];
            } else {
                result += static_cast<char>(ch);
            }
            break;
        }
    }
    return result;
}

void write_summary_json(const std::filesystem::path& path, const SummaryReport& report)
{
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Failed to open summary output file: " + path.string());
    }

    std::vector<std::string> fields;
    if (report.capture_path) {
        fields.push_back("  \"capture\": \"" + json_escape(*report.capture_path) + "\"");
    }
    if (report.metadata) {
        const auto& meta = *report.metadata;
        std::vector<std::string> meta_fields;
        meta_fields.push_back("    \"sf\": " + std::to_string(meta.sf));
        meta_fields.push_back("    \"bw\": " + std::to_string(meta.bw));
        meta_fields.push_back("    \"sample_rate\": " + std::to_string(meta.sample_rate));
        meta_fields.push_back("    \"cr\": " + std::to_string(meta.cr));
        meta_fields.push_back("    \"payload_len\": " + std::to_string(meta.payload_len));
        meta_fields.push_back("    \"preamble_len\": " + std::to_string(meta.preamble_len));
        meta_fields.push_back("    \"has_crc\": " + std::string(json_bool(meta.has_crc)));
        meta_fields.push_back("    \"implicit_header\": " + std::string(json_bool(meta.implicit_header)));
        meta_fields.push_back("    \"ldro\": " + std::string(json_bool(meta.ldro)));
        meta_fields.push_back("    \"sync_word\": " + std::to_string(meta.sync_word));
        if (meta.payload_hex && !meta.payload_hex->empty()) {
            meta_fields.push_back("    \"payload_hex\": \"" + json_escape(*meta.payload_hex) + "\"");
        }
        std::ostringstream meta_ss;
        meta_ss << "  \"metadata\": {\n";
        for (std::size_t i = 0; i < meta_fields.size(); ++i) {
            meta_ss << meta_fields[i];
            if (i + 1 < meta_fields.size()) {
                meta_ss << ',';
            }
            meta_ss << '\n';
        }
        meta_ss << "  }";
        fields.push_back(meta_ss.str());
    }
    if (report.stats) {
        const auto& st = *report.stats;
        std::ostringstream stats_ss;
        stats_ss << "  \"stats\": {\n";
        stats_ss << "    \"sample_count\": " << st.sample_count << ",\n";
        stats_ss << "    \"min_magnitude\": " << std::setprecision(6) << std::fixed << st.min_magnitude << ",\n";
        stats_ss << "    \"max_magnitude\": " << std::setprecision(6) << std::fixed << st.max_magnitude << ",\n";
        stats_ss << "    \"mean_power\": " << std::setprecision(6) << std::fixed << st.mean_power << "\n";
        stats_ss << "  }";
        fields.push_back(stats_ss.str());
    }
    fields.push_back("  \"reference_mismatches\": " + std::to_string(report.reference_mismatches));
    fields.push_back("  \"stage_mismatches\": " + std::to_string(report.stage_mismatches));
    fields.push_back("  \"whitening_roundtrip_ok\": " + std::string(json_bool(report.whitening_roundtrip_ok)));
    fields.push_back("  \"compare_run\": " + std::string(json_bool(report.compare_run)));

    if (!report.stage_timings_ns.empty()) {
        std::ostringstream timings_ss;
        timings_ss << "  \"stage_timings_ns\": [";
        for (std::size_t i = 0; i < report.stage_timings_ns.size(); ++i) {
            if (i > 0) {
                timings_ss << ", ";
            }
            timings_ss << report.stage_timings_ns[i];
        }
        timings_ss << "]";
        fields.push_back(timings_ss.str());
    }

    if (!report.memory_usage_bytes.empty()) {
        std::ostringstream mem_ss;
        mem_ss << "  \"symbol_memory_bytes\": [";
        for (std::size_t i = 0; i < report.memory_usage_bytes.size(); ++i) {
            if (i > 0) {
                mem_ss << ", ";
            }
            mem_ss << report.memory_usage_bytes[i];
        }
        mem_ss << "]";
        fields.push_back(mem_ss.str());
    }

    if (!report.preview_symbols.empty()) {
        std::ostringstream preview_ss;
        preview_ss << "  \"preview_symbols\": [";
        for (std::size_t i = 0; i < report.preview_symbols.size(); ++i) {
            if (i > 0) {
                preview_ss << ", ";
            }
            preview_ss << report.preview_symbols[i];
        }
        preview_ss << "]";
        fields.push_back(preview_ss.str());
    }

    {
        std::ostringstream stages_ss;
        if (report.stage_results.empty()) {
            stages_ss << "  \"stages\": []";
        } else {
            stages_ss << "  \"stages\": [\n";
            for (std::size_t i = 0; i < report.stage_results.size(); ++i) {
                const auto& res = report.stage_results[i];
                std::vector<std::string> stage_fields;
                stage_fields.push_back("      \"label\": \"" + json_escape(res.label) + "\"");
                stage_fields.push_back("      \"host\": " + std::to_string(res.host_count));
                stage_fields.push_back("      \"ref\": " + std::to_string(res.ref_count));
                stage_fields.push_back("      \"mismatches\": " + std::to_string(res.mismatches));
                stage_fields.push_back("      \"summary\": \"" + json_escape(build_stage_summary_token(res)) + "\"");
                stage_fields.push_back("      \"reference_missing\": " + std::string(json_bool(res.reference_missing)));
                if (res.alignment_offset) {
                    stage_fields.push_back("      \"alignment_offset\": " + std::to_string(*res.alignment_offset));
                }
                stage_fields.push_back("      \"alignment_relative_to_reference\": "
                                       + std::string(json_bool(res.alignment_relative_to_reference)));
                if (res.first_diff_index) {
                    stage_fields.push_back("      \"first_diff_index\": " + std::to_string(*res.first_diff_index));
                }
                if (res.host_value) {
                    stage_fields.push_back("      \"host_value\": " + std::to_string(*res.host_value));
                }
                if (res.ref_value) {
                    stage_fields.push_back("      \"ref_value\": " + std::to_string(*res.ref_value));
                }

                stages_ss << "    {\n";
                for (std::size_t j = 0; j < stage_fields.size(); ++j) {
                    stages_ss << stage_fields[j];
                    if (j + 1 < stage_fields.size()) {
                        stages_ss << ',';
                    }
                    stages_ss << '\n';
                }
                stages_ss << "    }";
                if (i + 1 < report.stage_results.size()) {
                    stages_ss << ',';
                }
                stages_ss << '\n';
            }
            stages_ss << "  ]";
        }
        fields.push_back(stages_ss.str());
    }

    out << "{\n";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        out << fields[i];
        if (i + 1 < fields.size()) {
            out << ',';
        }
        out << '\n';
    }
    out << "}\n";
}

std::vector<StageComparisonResult> compare_with_reference(const StageOutputs& outputs,
                                                          const std::filesystem::path& compare_root)
{
    std::filesystem::path base = compare_root;
    if (base.extension() == ".cf32") {
        base.replace_extension("");
    }

    std::vector<StageComparisonResult> results;
    auto compare_stage_file = [&](const char* suffix,
                                  const auto& host_vec,
                                  const char* label) {
        std::filesystem::path path = base;
        path += suffix;
        if (!std::filesystem::exists(path)) {
            StageComparisonResult missing;
            missing.label = label;
            missing.host_count = host_vec.size();
            missing.ref_count = 0;
            missing.mismatches = host_vec.size();
            missing.reference_missing = true;
            results.push_back(missing);
            return;
        }
        auto reference = read_stage_file(path);
        results.push_back(compare_stage(label, host_vec, reference));
    };

    compare_stage_file("_fft.txt", outputs.fft, "FFT");
    compare_stage_file("_gray.txt", outputs.gray, "Gray");
    compare_stage_file("_deinterleaver.txt", outputs.deinterleaver, "Deinterleaver");
    compare_stage_file("_hamming.txt", outputs.hamming, "Hamming");

    return results;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        std::cerr << "[debug] entering lora_replay" << std::endl;
        const Options options = parse_arguments(argc, argv);

        const auto samples = host_sim::load_cf32(options.iq_file);
        std::cerr << "[debug] loaded samples=" << samples.size() << std::endl;
        const auto stats = host_sim::analyse_capture(samples);
        std::cerr << "[debug] analysed capture" << std::endl;

        SummaryReport summary;
        if (!options.iq_file.empty()) {
            summary.capture_path = options.iq_file.generic_string();
        }
        summary.stats = stats;

        std::vector<uint16_t> symbols;
        std::vector<host_sim::SymbolLLR> symbol_llrs;
        std::size_t alignment_samples = 0;

        std::cout << "Loaded capture: " << options.iq_file << "\n"
                  << "  Samples: " << stats.sample_count << "\n"
                  << "  Min |x|: " << std::setprecision(6) << stats.min_magnitude << "\n"
                  << "  Max |x|: " << std::setprecision(6) << stats.max_magnitude << "\n"
                  << "  Mean power: " << std::setprecision(6) << stats.mean_power << "\n";

        std::cerr << "[debug] preparing metadata" << std::endl;
        std::optional<host_sim::LoRaMetadata> metadata;
        if (options.metadata) {
            metadata = host_sim::load_metadata(*options.metadata);
        } else {
            auto guess = options.iq_file;
            guess.replace_extension(".json");
            if (std::filesystem::exists(guess)) {
                metadata = host_sim::load_metadata(guess);
            }
        }
        std::cerr << "[debug] metadata loaded? " << (metadata.has_value()) << std::endl;
        if (metadata) {
            summary.metadata = *metadata;
        }

        bool compare_failure = false;
        std::size_t total_stage_mismatches = 0;
        std::size_t reference_mismatches = 0;

        if (metadata) {
            StageOutputs stage_outputs;
            bool have_stage_outputs = false;
            std::cout << "Metadata: SF=" << metadata->sf << ", CR=" << metadata->cr
                      << ", BW=" << metadata->bw << ", Fs=" << metadata->sample_rate
                      << ", payload_len=" << metadata->payload_len << "\n";
            std::cerr << "[debug] after metadata print" << std::endl;

            host_sim::FftDemodulator demod(metadata->sf, metadata->sample_rate, metadata->bw);
            host_sim::FftDemodReference demod_ref(metadata->sf, metadata->sample_rate, metadata->bw);
            std::cerr << "[debug] demodulators constructed" << std::endl;
            const int sps = demod.samples_per_symbol();
            int detected_preamble_bin = 0;

            // --- Stage 0: burst detection ---
            // If the capture contains noise before the signal, skip to
            // the burst start so that alignment search sees the preamble.
            std::size_t multi_search_offset = 0;
            int multi_packet_index = 0;
            bool multi_any_failed = false;

          for (;;) { // multi-packet loop (runs once unless --multi)
            const auto burst_result = host_sim::detect_burst_start(
                samples, sps, 6.0f, multi_search_offset);
            const std::size_t burst_offset = burst_result.value_or(0);
            if (!burst_result && multi_search_offset > 0) {
                break; // no more bursts in --multi mode
            }
            if (burst_offset > 0) {
                if (options.multi_packet) {
                    std::cout << "\n=== Packet #" << multi_packet_index << " ===\n";
                }
                std::cout << "Burst detected at sample " << burst_offset
                          << " (" << static_cast<double>(burst_offset) /
                                     metadata->sample_rate * 1000.0
                          << " ms)" << std::endl;
            }

            // Create a view of samples starting at the burst.
            const std::complex<float>* burst_samples = samples.data() + burst_offset;
            const std::size_t burst_len = samples.size() - burst_offset;
            // Wrap in a temporary vector for alignment functions that
            // take vector reference (no copy — we'll build a lightweight span).
            const std::vector<std::complex<float>> burst_view(
                burst_samples, burst_samples + burst_len);

            // --- Stage 1: alignment ---
            // At low oversampling (os<=4), the polyphase anti-aliasing is weak
            // so legacy alignment (bin-0 scoring) is more reliable.  At high
            // oversampling (os>4), the legacy approach fails due to aliasing,
            // so we use the CFO-aware alignment (magnitude scoring).
            const int os = demod.oversample_factor();
            if (os > 4) {
                auto pr = host_sim::find_symbol_alignment_cfo_aware(
                    burst_view, demod, metadata->preamble_len);
                alignment_samples = burst_offset + pr.alignment_offset;
                detected_preamble_bin = pr.preamble_bin;
                std::cout << "Alignment offset: " << alignment_samples << " samples"
                          << " (preamble_bin=" << pr.preamble_bin
                          << ", score=" << pr.score << ")" << std::endl;
            } else {
                // Legacy alignment: fast, works for zero/small CFO.
                alignment_samples = burst_offset +
                    host_sim::find_symbol_alignment(burst_view, demod,
                                                    metadata->preamble_len);
                std::cout << "Alignment offset: " << alignment_samples
                          << " samples" << std::endl;
            }
            std::cerr << "[debug] alignment done" << std::endl;

            const int available_symbols = static_cast<int>(
                (samples.size() > alignment_samples
                     ? (samples.size() - alignment_samples) / sps
                     : 0));
            const int preamble_symbols_to_use =
                std::min(std::max(metadata->preamble_len - 1, 0), available_symbols);
            if (preamble_symbols_to_use > 0) {
                auto freq_est = demod.estimate_frequency_offsets(
                    samples.data() + alignment_samples,
                    preamble_symbols_to_use);
                // If the CFO-aware search detected a large preamble bin that
                // estimate_frequency_offsets missed, use the detected value.
                if (detected_preamble_bin != 0) {
                    const int n_bins = 1 << metadata->sf;
                    int signed_bin = detected_preamble_bin;
                    if (signed_bin > n_bins / 2) signed_bin -= n_bins;
                    if (std::abs(signed_bin) > std::abs(freq_est.cfo_int) + 2) {
                        freq_est.cfo_int = signed_bin;
                    }
                }
                demod.set_frequency_offsets(freq_est.cfo_frac,
                                            freq_est.cfo_int,
                                            freq_est.sfo_slope);

                // --- Sub-sample alignment refinement ---
                // The alignment algorithm may be off by a few samples.
                // At low oversampling (os ≤ 4), even a 1–2 sample error
                // shifts the dechirped FFT peak by a fraction of a bin,
                // which can flip the rounded bin on marginal symbols.
                // Try a few offsets around the detected alignment and pick
                // the one whose preamble symbols most agree on bin 0.
                if (os <= 4 && !options.compare_root) {
                    const int n_bins = 1 << metadata->sf;
                    int best_offset = 0;
                    int best_count_0 = -1;
                    for (int try_off = -3; try_off <= 3; ++try_off) {
                        const auto try_align = static_cast<std::size_t>(
                            static_cast<std::ptrdiff_t>(alignment_samples) + try_off);
                        if (try_align + 8ULL * sps > samples.size()) continue;
                        demod.set_frequency_offsets(freq_est.cfo_frac,
                                                    freq_est.cfo_int,
                                                    freq_est.sfo_slope);
                        demod.reset_symbol_counter();
                        int c0 = 0;
                        for (int p = 0; p < std::min(preamble_symbols_to_use, 8); ++p) {
                            uint16_t v = demod.demodulate(
                                &samples[try_align +
                                         static_cast<std::size_t>(p) * sps]);
                            if (v == 0) ++c0;
                        }
                        if (c0 > best_count_0) {
                            best_count_0 = c0;
                            best_offset = try_off;
                        }
                    }
                    if (best_offset != 0) {
                        alignment_samples = static_cast<std::size_t>(
                            static_cast<std::ptrdiff_t>(alignment_samples) +
                            best_offset);
                    }
                }

                demod.set_frequency_offsets(freq_est.cfo_frac,
                                            freq_est.cfo_int,
                                            freq_est.sfo_slope);
                demod_ref.set_frequency_offsets(freq_est.cfo_frac,
                                                freq_est.cfo_int,
                                                freq_est.sfo_slope);
                demod.reset_symbol_counter();
                // Per-symbol CFO tracking: disabled by default.
                // Enable with HOST_SIM_CFO_TRACK_ALPHA env variable.
                if (const char* alpha_env = std::getenv("HOST_SIM_CFO_TRACK_ALPHA")) {
                    demod.set_cfo_tracking(std::stof(alpha_env));
                }
                std::cout << "Frequency offsets: CFO_int=" << freq_est.cfo_int
                          << " CFO_frac=" << freq_est.cfo_frac
                          << " SFO_slope=" << freq_est.sfo_slope << "\n";
            } else {
                demod.set_frequency_offsets(0.0f, 0, 0.0f);
                demod_ref.set_frequency_offsets(0.0f, 0.0f, 0);
                demod.reset_symbol_counter();
            }

            const int usable_samples = static_cast<int>(samples.size() > alignment_samples ? samples.size() - alignment_samples : 0);
            const int symbol_count = usable_samples / sps;
            symbols.reserve(symbol_count);
            for (int idx = 0; idx < symbol_count; ++idx) {
                uint16_t value = demod.demodulate(&samples[alignment_samples + static_cast<std::size_t>(idx) * sps]);
                symbols.push_back(value);
                if (options.soft) {
                    auto mags = demod.get_fft_magnitudes_sq();
                    symbol_llrs.push_back(host_sim::compute_symbol_llrs(
                        mags.data(), metadata->sf,
                        (idx < 8) || metadata->ldro,
                        demod.current_cfo_int()));
                }
                uint16_t ref_value = demod_ref.demodulate(&samples[alignment_samples + static_cast<std::size_t>(idx) * sps]);
                if (value != ref_value) {
                    ++reference_mismatches;
                    if (reference_mismatches <= 8) {
                        std::cout << "[reference] mismatch symbol " << idx
                                  << " host=" << value << " ref=" << ref_value << "\n";
                    }
                }
            }

            if (symbol_count == 0) {
                std::cout << "[reference] no symbols available for comparison\n";
            } else if (reference_mismatches > 0) {
                compare_failure = true;
                std::cout << "[reference] total mismatched symbols: " << reference_mismatches << "\n";
            } else {
                std::cout << "[reference] symbols match reference demodulator\n";
            }
            summary.reference_mismatches = reference_mismatches;

        std::cout << "First 16 aligned symbols:";
        for (int i = 0; i < std::min<int>(16, symbols.size()); ++i) {
            std::cout << ' ' << symbols[i];
        }
        std::cout << "\n";
        if (symbols.size() >= 32) {
            std::cout << "Aligned symbols (0-31):";
            for (int i = 0; i < 32; ++i) {
                std::cout << ' ' << symbols[i];
            }
            std::cout << "\n";
        }
            summary.preview_symbols.assign(
                symbols.begin(),
                symbols.begin() + std::min<std::size_t>(symbols.size(), static_cast<std::size_t>(32)));

            HeaderDecodeResult header;
            std::size_t symbol_cursor = 0;
            std::size_t chosen_offset = std::numeric_limits<std::size_t>::max();

            // At high oversampling (os > 4) or BW500-class captures
            // (os == 4 in non-comparison mode), the LoRa SFD's 0.25-symbol
            // offset shifts data symbols off the preamble grid.  Scanning
            // preamble-grid symbols for a header is futile and can yield
            // false positives from noise, so skip straight to the SFD
            // re-demod path.  Stage-comparison tests at os == 4 still need
            // the preamble-grid scan to stay bit-exact with the reference.
            const bool skip_grid_scan =
                (os > 4) || (os == 4 && !options.compare_root);
            if (!skip_grid_scan) {
            for (std::size_t candidate = 0; candidate + 8 <= symbols.size(); ++candidate) {
                auto candidate_header = try_decode_header(symbols, candidate, *metadata);
                if (!candidate_header.success) {
                    continue;
                }

                int header_len = candidate_header.payload_len;
                if (header_len <= 0) {
                    header_len = metadata->payload_len;
                }
                if (metadata->payload_len > 0 && header_len != metadata->payload_len) {
                    continue;
                }

                int header_cr = candidate_header.cr;
                if (header_cr <= 0) {
                    header_cr = metadata->cr;
                }
                if (metadata->cr > 0 && header_cr != metadata->cr) {
                    continue;
                }

                if (metadata->has_crc && !candidate_header.has_crc) {
                    continue;
                }

                header = std::move(candidate_header);
                symbol_cursor = candidate + header.consumed_symbols;
                chosen_offset = candidate;
                break;
            }
            } // !skip_grid_scan

            // Fallback: if header not found on the preamble grid, try
            // re-demodulating from the sync word position with a quarter-
            // symbol SFD offset.  The LoRa SFD is 2.25 downchirps, so data
            // symbols start at sync + 2 (sync) + 2.25 (SFD) = sync + 4.25
            // symbols.  At high oversampling this 0.25 offset shifts the bin
            // by N/4, making header decode impossible on the preamble grid.
            // At os=1, the 0.25 offset is still non-zero (sps/4 samples),
            // so this fallback is needed at ALL oversampling factors.
            // Exception: stage-comparison mode (--compare-root) needs the
            // preamble-grid decode to stay bit-exact with the reference.
            if (!header.success && !options.compare_root) {
                auto sync_pos = host_sim::find_header_symbol_index(
                    symbols, 0x12, metadata->sf);
                if (!sync_pos) {
                    sync_pos = host_sim::find_header_symbol_index(
                        symbols, 0x34, metadata->sf);
                }

                // Save demod state for re-demod attempts (needed by
                // both the native-OS and the OS=2-upsample fallback).
                const float saved_cfo_frac = demod.current_cfo_frac();
                const int   saved_cfo_int  = demod.current_cfo_int();
                const float saved_sfo      = demod.current_sfo_slope();

                if (sync_pos) {
                    const std::size_t quarter = static_cast<std::size_t>(sps / 4);
                    const int n_bins = 1 << metadata->sf;

                    // Try quarter-symbol offsets: 1 (standard), 0, 2, 3
                    for (int qoff : {1, 0, 2, 3}) {
                        if (header.success) break;
                        const std::size_t data_sample = alignment_samples +
                            *sync_pos * static_cast<std::size_t>(sps) +
                            static_cast<std::size_t>(qoff) * quarter;
                        if (data_sample + 8ULL * sps > samples.size()) continue;

                        // Re-demodulate data symbols from adjusted position.
                        demod.set_frequency_offsets(saved_cfo_frac,
                                                   saved_cfo_int,
                                                   saved_sfo);
                        demod.reset_symbol_counter();
                        std::vector<uint16_t> redemod;
                        std::vector<host_sim::SymbolLLR> redemod_llrs;
                        const std::size_t max_sym = (samples.size() - data_sample) / sps;
                        // Cap at 1024 symbols: enough for max LoRa payload (255B, any SF/CR)
                        // while preventing multi-packet mode from consuming the entire capture.
                        for (std::size_t i = 0; i < std::min<std::size_t>(max_sym, 1024); ++i) {
                            redemod.push_back(demod.demodulate(
                                &samples[data_sample + i * sps]));
                            if (options.soft) {
                                auto mags = demod.get_fft_magnitudes_sq();
                                redemod_llrs.push_back(host_sim::compute_symbol_llrs(
                                    mags.data(), metadata->sf,
                                    (static_cast<int>(i) < 8) || metadata->ldro,
                                    saved_cfo_int));
                            }
                        }

                        if (metadata->implicit_header) {
                            // Helper lambda: build implicit header from symbols.
                            auto build_implicit_header = [&](const std::vector<uint16_t>& syms)
                                -> HeaderDecodeResult {
                                HeaderDecodeResult h;
                                const std::size_t hdr_syms = std::min<std::size_t>(8, syms.size());
                                std::vector<uint16_t> first_block(syms.begin(),
                                                                  syms.begin() + hdr_syms);
                                host_sim::DeinterleaverConfig hdr_cfg{metadata->sf, 4, true, metadata->ldro};
                                std::size_t consumed_block = 0;
                                auto codewords = host_sim::deinterleave(first_block, hdr_cfg, consumed_block);
                                auto nibs = host_sim::hamming_decode_block(codewords, true, 4);
                                h.success = true;
                                h.payload_len = metadata->payload_len;
                                h.cr = metadata->cr;
                                h.has_crc = metadata->has_crc;
                                h.checksum_field = -1;
                                h.checksum_computed = -1;
                                h.consumed_symbols = consumed_block > 0 ? consumed_block : 8;
                                h.codewords.assign(codewords.begin(), codewords.end());
                                h.nibbles.assign(5, 0);
                                h.nibbles.insert(h.nibbles.end(), nibs.begin(), nibs.end());
                                return h;
                            };

                            auto imp_hdr = build_implicit_header(redemod);

                            // CRC-guided timing sweep for implicit header.
                            if (metadata->has_crc &&
                                !probe_payload_crc(redemod, imp_hdr, *metadata)) {
                                const int max_adj = std::max(os, 4) + 2;
                                for (int adj = -1; std::abs(adj) <= max_adj;
                                     adj = adj > 0 ? -adj - 1 : -adj) {
                                    const auto adj_data = static_cast<std::size_t>(
                                        static_cast<std::ptrdiff_t>(data_sample) + adj);
                                    if (adj_data + 8ULL * sps > samples.size())
                                        continue;
                                    demod.set_frequency_offsets(saved_cfo_frac,
                                                               saved_cfo_int,
                                                               saved_sfo);
                                    demod.reset_symbol_counter();
                                    std::vector<uint16_t> adj_syms;
                                    std::vector<host_sim::SymbolLLR> adj_llrs;
                                    const std::size_t adj_max =
                                        (samples.size() - adj_data) / sps;
                                    for (std::size_t i = 0;
                                         i < std::min<std::size_t>(adj_max, 200);
                                         ++i) {
                                        adj_syms.push_back(demod.demodulate(
                                            &samples[adj_data + i * sps]));
                                        if (options.soft) {
                                            auto mags = demod.get_fft_magnitudes_sq();
                                            adj_llrs.push_back(host_sim::compute_symbol_llrs(
                                                mags.data(), metadata->sf,
                                                (static_cast<int>(i) < 8) || metadata->ldro,
                                                saved_cfo_int));
                                        }
                                    }
                                    auto adj_hdr = build_implicit_header(adj_syms);
                                    if (probe_payload_crc(adj_syms, adj_hdr,
                                                          *metadata)) {
                                        redemod = std::move(adj_syms);
                                        redemod_llrs = std::move(adj_llrs);
                                        imp_hdr = std::move(adj_hdr);
                                        std::cout << "SFD re-demod: implicit data start "
                                                     "refined by "
                                                  << adj
                                                  << " samples (CRC verified)\n";
                                        break;
                                    }
                                }
                            }

                            // Only accept this qoff if CRC validated
                            // (or if there's no CRC to check).
                            bool crc_ok = !metadata->has_crc ||
                                probe_payload_crc(redemod, imp_hdr, *metadata);

                            if (crc_ok) {
                                std::cout << "SFD re-demod: implicit header, quarter offset "
                                          << qoff << " (sync at symbol " << (*sync_pos - 4) << ")\n";
                                header = std::move(imp_hdr);
                                symbol_cursor = header.consumed_symbols;
                                chosen_offset = 0;
                                symbols = std::move(redemod);
                                symbol_llrs = std::move(redemod_llrs);
                                break;
                            }
                            // CRC failed even after timing sweep — try next qoff.
                            continue;
                        }

                        auto hdr = try_decode_header(redemod, 0, *metadata);
                        if (!hdr.success) continue;

                        int hlen = hdr.payload_len > 0 ? hdr.payload_len : metadata->payload_len;
                        int hcr = hdr.cr > 0 ? hdr.cr : metadata->cr;
                        if (metadata->payload_len > 0 && hlen != metadata->payload_len) continue;
                        if (metadata->cr > 0 && hcr != metadata->cr) continue;
                        if (metadata->has_crc && !hdr.has_crc) continue;

                        // SFO-induced timing drift between the preamble
                        // and data start can shift data symbols by ±1 bin.
                        // The header is robust (uses sf_app = sf-2) but the
                        // payload is not.  Sweep small timing adjustments
                        // around the nominal data_sample and select the one
                        // that gives a valid CRC.
                        bool timing_refined = false;
                        if ((hdr.has_crc || metadata->has_crc) &&
                            !probe_payload_crc(redemod, hdr, *metadata)) {
                            const int max_adj = std::max(os, 4) + 2;
                            for (int adj = -1; std::abs(adj) <= max_adj; adj = adj > 0 ? -adj - 1 : -adj) {
                                const auto adj_data = static_cast<std::size_t>(
                                    static_cast<std::ptrdiff_t>(data_sample) + adj);
                                if (adj_data + 8ULL * sps > samples.size())
                                    continue;
                                demod.set_frequency_offsets(saved_cfo_frac,
                                                           saved_cfo_int,
                                                           saved_sfo);
                                demod.reset_symbol_counter();
                                std::vector<uint16_t> adj_syms;
                                std::vector<host_sim::SymbolLLR> adj_llrs;
                                const std::size_t adj_max =
                                    (samples.size() - adj_data) / sps;
                                for (std::size_t i = 0;
                                     i < std::min<std::size_t>(adj_max, 200);
                                     ++i) {
                                    adj_syms.push_back(demod.demodulate(
                                        &samples[adj_data + i * sps]));
                                    if (options.soft) {
                                        auto mags = demod.get_fft_magnitudes_sq();
                                        adj_llrs.push_back(host_sim::compute_symbol_llrs(
                                            mags.data(), metadata->sf,
                                            (static_cast<int>(i) < 8) || metadata->ldro,
                                            saved_cfo_int));
                                    }
                                }
                                auto adj_hdr =
                                    try_decode_header(adj_syms, 0, *metadata);
                                if (!adj_hdr.success) continue;
                                if (probe_payload_crc(adj_syms, adj_hdr,
                                                      *metadata)) {
                                    redemod = std::move(adj_syms);
                                    redemod_llrs = std::move(adj_llrs);
                                    hdr = std::move(adj_hdr);
                                    timing_refined = true;
                                    std::cout << "SFD re-demod: data start "
                                                 "refined by "
                                              << adj
                                              << " samples (CRC verified)\n";
                                    break;
                                }
                            }
                        }

                        std::cout << "SFD re-demod: header found with quarter offset "
                                  << qoff << " (sync at symbol " << (*sync_pos - 4)
                                  << ")\n";
                        header = std::move(hdr);
                        symbol_cursor = header.consumed_symbols;
                        chosen_offset = 0;
                        symbols = std::move(redemod);
                        symbol_llrs = std::move(redemod_llrs);
                        break;
                    }
                }

                // ── OS=2 upsample fallback ──────────────────────────
                // When the native-OS decode at OS=1 either fails or
                // produces a CRC-invalid payload, upsample the burst by
                // 2x and retry.  This doubles the SFO timing tolerance
                // (n_symbols × sfo_ppm threshold ~4000 vs ~2000 at OS=1).
                // If OS=2 also fails CRC, the original decode is kept.
                bool need_os2 = !header.success;
                if (!need_os2 && os == 1 && sync_pos &&
                    (header.has_crc || metadata->has_crc) &&
                    !probe_payload_crc(symbols, header, *metadata)) {
                    need_os2 = true;
                }
                if (need_os2 && sync_pos && os == 1) {
                    // Save current decode as fallback.
                    auto fallback_header = header;
                    auto fallback_symbols = symbols;
                    auto fallback_llrs = symbol_llrs;
                    auto fallback_cursor = symbol_cursor;
                    auto fallback_offset = chosen_offset;
                    header.success = false;

                    const std::size_t burst_start = alignment_samples;
                    const std::size_t burst_len = samples.size() - burst_start;
                    auto up = upsample_2x(&samples[burst_start], burst_len);

                    const int sps_os2 = sps * 2;
                    const std::size_t quarter_os2 =
                        static_cast<std::size_t>(sps_os2 / 4);
                    host_sim::FftDemodulator demod_os2(metadata->sf,
                                            metadata->bw * 2,
                                            metadata->bw);

                    for (int qoff : {1, 0, 2, 3}) {
                        if (header.success) break;
                        const std::size_t data_sample_os2 =
                            *sync_pos * static_cast<std::size_t>(sps_os2) +
                            static_cast<std::size_t>(qoff) * quarter_os2;
                        if (data_sample_os2 + 8ULL * sps_os2 > up.size())
                            continue;

                        demod_os2.set_frequency_offsets(saved_cfo_frac,
                                                       saved_cfo_int,
                                                       0.0f);
                        demod_os2.reset_symbol_counter();
                        std::vector<uint16_t> redemod;
                        std::vector<host_sim::SymbolLLR> redemod_llrs;
                        const std::size_t max_sym =
                            (up.size() - data_sample_os2) / sps_os2;
                        for (std::size_t i = 0;
                             i < std::min<std::size_t>(max_sym, 1024); ++i) {
                            redemod.push_back(demod_os2.demodulate(
                                &up[data_sample_os2 +
                                    i * static_cast<std::size_t>(sps_os2)]));
                            if (options.soft) {
                                auto mags = demod_os2.get_fft_magnitudes_sq();
                                redemod_llrs.push_back(
                                    host_sim::compute_symbol_llrs(
                                        mags.data(), metadata->sf,
                                        (static_cast<int>(i) < 8) ||
                                            metadata->ldro,
                                        saved_cfo_int));
                            }
                        }

                        if (metadata->implicit_header) {
                            HeaderDecodeResult imp_hdr;
                            {
                                const std::size_t hdr_syms =
                                    std::min<std::size_t>(8, redemod.size());
                                std::vector<uint16_t> first_block(
                                    redemod.begin(),
                                    redemod.begin() + hdr_syms);
                                host_sim::DeinterleaverConfig hdr_cfg{
                                    metadata->sf, 4, true, metadata->ldro};
                                std::size_t consumed_block = 0;
                                auto codewords = host_sim::deinterleave(
                                    first_block, hdr_cfg, consumed_block);
                                auto nibs =
                                    host_sim::hamming_decode_block(
                                        codewords, true, 4);
                                imp_hdr.success = true;
                                imp_hdr.payload_len = metadata->payload_len;
                                imp_hdr.cr = metadata->cr;
                                imp_hdr.has_crc = metadata->has_crc;
                                imp_hdr.checksum_field = -1;
                                imp_hdr.checksum_computed = -1;
                                imp_hdr.consumed_symbols =
                                    consumed_block > 0 ? consumed_block : 8;
                                imp_hdr.codewords.assign(
                                    codewords.begin(), codewords.end());
                                imp_hdr.nibbles.assign(5, 0);
                                imp_hdr.nibbles.insert(
                                    imp_hdr.nibbles.end(),
                                    nibs.begin(), nibs.end());
                            }

                            if (metadata->has_crc &&
                                !probe_payload_crc(redemod, imp_hdr,
                                                   *metadata)) {
                                for (int adj = -1; std::abs(adj) <= 6;
                                     adj = adj > 0 ? -adj - 1 : -adj) {
                                    const auto adj_data =
                                        static_cast<std::size_t>(
                                            static_cast<std::ptrdiff_t>(
                                                data_sample_os2) +
                                            adj);
                                    if (adj_data + 8ULL * sps_os2 >
                                        up.size())
                                        continue;
                                    demod_os2.set_frequency_offsets(
                                        saved_cfo_frac, saved_cfo_int,
                                        0.0f);
                                    demod_os2.reset_symbol_counter();
                                    std::vector<uint16_t> adj_syms;
                                    const std::size_t adj_max =
                                        (up.size() - adj_data) / sps_os2;
                                    for (std::size_t i = 0;
                                         i < std::min<std::size_t>(
                                                 adj_max, 1024);
                                         ++i) {
                                        adj_syms.push_back(
                                            demod_os2.demodulate(
                                                &up[adj_data +
                                                    i * static_cast<
                                                            std::size_t>(
                                                            sps_os2)]));
                                    }
                                    HeaderDecodeResult adj_imp;
                                    {
                                        const std::size_t hs =
                                            std::min<std::size_t>(
                                                8, adj_syms.size());
                                        std::vector<uint16_t> fb(
                                            adj_syms.begin(),
                                            adj_syms.begin() + hs);
                                        host_sim::DeinterleaverConfig
                                            hcfg{metadata->sf, 4, true,
                                                 metadata->ldro};
                                        std::size_t cb = 0;
                                        auto cw =
                                            host_sim::deinterleave(
                                                fb, hcfg, cb);
                                        auto nb =
                                            host_sim::hamming_decode_block(
                                                cw, true, 4);
                                        adj_imp.success = true;
                                        adj_imp.payload_len =
                                            metadata->payload_len;
                                        adj_imp.cr = metadata->cr;
                                        adj_imp.has_crc =
                                            metadata->has_crc;
                                        adj_imp.checksum_field = -1;
                                        adj_imp.checksum_computed = -1;
                                        adj_imp.consumed_symbols =
                                            cb > 0 ? cb : 8;
                                        adj_imp.codewords.assign(
                                            cw.begin(), cw.end());
                                        adj_imp.nibbles.assign(5, 0);
                                        adj_imp.nibbles.insert(
                                            adj_imp.nibbles.end(),
                                            nb.begin(), nb.end());
                                    }
                                    if (probe_payload_crc(adj_syms, adj_imp,
                                                          *metadata)) {
                                        redemod = std::move(adj_syms);
                                        imp_hdr = std::move(adj_imp);
                                        std::cout
                                            << "OS=2 upsample: implicit "
                                               "data start refined by "
                                            << adj
                                            << " samples (CRC verified)\n";
                                        break;
                                    }
                                }
                            }

                            bool crc_ok = !metadata->has_crc ||
                                probe_payload_crc(redemod, imp_hdr,
                                                  *metadata);
                            if (crc_ok) {
                                std::cout
                                    << "OS=2 upsample: implicit header, "
                                       "quarter offset "
                                    << qoff << "\n";
                                header = std::move(imp_hdr);
                                symbol_cursor = header.consumed_symbols;
                                chosen_offset = 0;
                                symbols = std::move(redemod);
                                symbol_llrs = std::move(redemod_llrs);
                                break;
                            }
                            continue;
                        }

                        auto hdr_os2 =
                            try_decode_header(redemod, 0, *metadata);
                        if (!hdr_os2.success) continue;

                        int hlen = hdr_os2.payload_len > 0
                                       ? hdr_os2.payload_len
                                       : metadata->payload_len;
                        int hcr = hdr_os2.cr > 0 ? hdr_os2.cr
                                                  : metadata->cr;
                        if (metadata->payload_len > 0 &&
                            hlen != metadata->payload_len)
                            continue;
                        if (metadata->cr > 0 && hcr != metadata->cr)
                            continue;
                        if (metadata->has_crc && !hdr_os2.has_crc)
                            continue;

                        if ((hdr_os2.has_crc || metadata->has_crc) &&
                            !probe_payload_crc(redemod, hdr_os2,
                                               *metadata)) {
                            for (int adj = -1; std::abs(adj) <= 6;
                                 adj = adj > 0 ? -adj - 1 : -adj) {
                                const auto adj_data =
                                    static_cast<std::size_t>(
                                        static_cast<std::ptrdiff_t>(
                                            data_sample_os2) +
                                        adj);
                                if (adj_data + 8ULL * sps_os2 > up.size())
                                    continue;
                                demod_os2.set_frequency_offsets(
                                    saved_cfo_frac, saved_cfo_int, 0.0f);
                                demod_os2.reset_symbol_counter();
                                std::vector<uint16_t> adj_syms;
                                const std::size_t adj_max =
                                    (up.size() - adj_data) / sps_os2;
                                for (std::size_t i = 0;
                                     i < std::min<std::size_t>(adj_max,
                                                               1024);
                                     ++i) {
                                    adj_syms.push_back(
                                        demod_os2.demodulate(
                                            &up[adj_data +
                                                i * static_cast<
                                                        std::size_t>(
                                                        sps_os2)]));
                                }
                                auto adj_hdr = try_decode_header(
                                    adj_syms, 0, *metadata);
                                if (!adj_hdr.success) continue;
                                if (probe_payload_crc(adj_syms, adj_hdr,
                                                      *metadata)) {
                                    redemod = std::move(adj_syms);
                                    hdr_os2 = std::move(adj_hdr);
                                    std::cout
                                        << "OS=2 upsample: data start "
                                           "refined by "
                                        << adj
                                        << " samples (CRC verified)\n";
                                    break;
                                }
                            }
                        }

                        std::cout << "OS=2 upsample: header found with "
                                     "quarter offset "
                                  << qoff << "\n";
                        header = std::move(hdr_os2);
                        symbol_cursor = header.consumed_symbols;
                        chosen_offset = 0;
                        symbols = std::move(redemod);
                        symbol_llrs = std::move(redemod_llrs);
                        break;
                    }

                    // If OS=2 didn't produce CRC-valid decode, restore
                    // the original native-OS result.
                    if (!header.success && fallback_header.success) {
                        header = std::move(fallback_header);
                        symbols = std::move(fallback_symbols);
                        symbol_llrs = std::move(fallback_llrs);
                        symbol_cursor = fallback_cursor;
                        chosen_offset = fallback_offset;
                    }
                }
            }

            if (header.success) {
                std::cout << "Header located at symbol index: " << chosen_offset << "\n";
                std::cout << "Header (nibbles):";
                for (auto nib : header.nibbles) {
                    std::cout << ' ' << std::hex << static_cast<int>(nib) << std::dec;
                }
                std::cout << "\n";
                std::cout << "Decoded header -> payload_len=" << header.payload_len
                          << " crc=" << header.has_crc
                          << " cr=" << header.cr
                          << " checksum_field=" << header.checksum_field
                          << " checksum_calc=" << header.checksum_computed << "\n";

                int expected_payload_len = header.payload_len > 0 ? header.payload_len : metadata->payload_len;
                bool expected_crc = header.has_crc;
                if (metadata->has_crc && !header.has_crc) {
                    expected_crc = true;
                }
                const int active_cr = header.cr > 0 ? header.cr : metadata->cr;

                const std::size_t header_symbol_span = std::min<std::size_t>(
                    header.consumed_symbols > 0 ? header.consumed_symbols : static_cast<std::size_t>(8),
                    symbols.size() - chosen_offset);
                std::vector<uint16_t> header_block(symbols.begin() + chosen_offset,
                                                   symbols.begin() + chosen_offset + header_symbol_span);
                host_sim::DeinterleaverConfig header_cfg{metadata->sf, 4, true, metadata->ldro};

                have_stage_outputs = true;
                const int header_stage_symbols = header_cfg.is_header ? 8 : header_cfg.cr + 4;
                const std::size_t used_symbols =
                    std::min<std::size_t>(static_cast<std::size_t>(header_stage_symbols), header_block.size());
                std::vector<uint16_t> header_block_stage(header_block.begin(),
                                                         header_block.begin() + used_symbols);
                append_fft_gray(header_block_stage, true, metadata->ldro, metadata->sf, stage_outputs);

                const std::size_t stage_codewords = std::min<std::size_t>(
                    header.codewords.size(),
                    static_cast<std::size_t>(std::max(metadata->sf - 2, 0)));
                for (std::size_t i = 0; i < stage_codewords; ++i) {
                    stage_outputs.deinterleaver.push_back(static_cast<uint16_t>(header.codewords[i]));
                }

                for (std::size_t i = 0; i < stage_codewords && i < header.nibbles.size(); ++i) {
                    stage_outputs.hamming.push_back(static_cast<uint8_t>(header.nibbles[i] & 0xF));
                }

                std::size_t nibble_target = static_cast<std::size_t>(expected_payload_len) * 2;
                if (expected_crc) {
                    nibble_target += 4; // 2 bytes CRC
                }

                host_sim::DeinterleaverConfig payload_cfg{metadata->sf, active_cr, false, metadata->ldro};
                std::vector<uint8_t> payload_nibbles;

                // For SF >= 8, the header block (8 symbols at CR=4/8)
                // produces SF-2 nibbles: the first 5 are header fields,
                // and the remaining SF-7 are the start of the payload.
                // These must be prepended to the payload nibble stream.
                if (header.nibbles.size() > 5) {
                    for (std::size_t i = 5; i < header.nibbles.size(); ++i) {
                        payload_nibbles.push_back(header.nibbles[i]);
                    }
                }

                const int payload_cw_len = active_cr + 4;
                const bool suppress_payload_stage = (metadata->sf <= 6);
                while (symbol_cursor + payload_cw_len <= symbols.size() && payload_nibbles.size() < nibble_target) {
                    std::vector<uint16_t> block(symbols.begin() + symbol_cursor,
                                                symbols.begin() + symbol_cursor + payload_cw_len);
                    std::size_t consumed_block = 0;

                    std::vector<uint8_t> nibs;
                    std::vector<uint8_t> codewords;
                    if (options.soft && symbol_cursor + payload_cw_len <= symbol_llrs.size()) {
                        std::vector<host_sim::SymbolLLR> block_llrs(
                            symbol_llrs.begin() + symbol_cursor,
                            symbol_llrs.begin() + symbol_cursor + payload_cw_len);
                        nibs = host_sim::soft_decode_block(
                            block_llrs, metadata->sf, active_cr,
                            false, metadata->ldro, consumed_block);
                        // Hard deinterleave for stage outputs
                        codewords = host_sim::deinterleave(block, payload_cfg, consumed_block);
                    } else {
                        codewords = host_sim::deinterleave(block, payload_cfg, consumed_block);
                        nibs = host_sim::hamming_decode_block(codewords, false, active_cr);
                    }

                    if (consumed_block == 0) {
                        break;
                    }
                    symbol_cursor += consumed_block;
                    if (!suppress_payload_stage) {
                        append_fft_gray(block, false, metadata->ldro, metadata->sf, stage_outputs);
                    }
                    payload_nibbles.insert(payload_nibbles.end(), nibs.begin(), nibs.end());
                    if (!suppress_payload_stage) {
                        for (auto cw : codewords) {
                            stage_outputs.deinterleaver.push_back(static_cast<uint16_t>(cw));
                        }
                        for (auto nib : nibs) {
                            stage_outputs.hamming.push_back(static_cast<uint8_t>(nib & 0xF));
                        }
                    }
                }

                // Dewhiten nibbles first, then pack into bytes (matching GNU Radio's approach)
                // In gr-lora_sdr dewhitening_impl.cc:
                //   low_nib = in[2*i] ^ (whitening_seq[offset] & 0x0F);
                //   high_nib = in[2*i+1] ^ (whitening_seq[offset] >> 4);
                //   byte = high_nib << 4 | low_nib;
                // Note: nibble order is: [0]=low_nib, [1]=high_nib
                host_sim::WhiteningSequencer seq;
                auto whitening = seq.sequence(payload_nibbles.size() / 2);
                
                std::vector<uint8_t> unwhitened;
                unwhitened.reserve(payload_nibbles.size() / 2);
                for (std::size_t i = 0; i + 1 < payload_nibbles.size(); i += 2) {
                    std::size_t byte_idx = i / 2;
                    // GNU Radio convention: first nibble is low_nib, second is high_nib
                    // Note: CRC bytes are NOT dewhitened (matching GNU Radio behavior)
                    uint8_t low_nib, high_nib;
                    if (byte_idx < expected_payload_len) {
                        // Payload bytes: apply dewhitening
                        uint8_t w = (byte_idx < whitening.size()) ? whitening[byte_idx] : 0;
                        low_nib = (payload_nibbles[i] & 0xF) ^ (w & 0x0F);
                        high_nib = (payload_nibbles[i + 1] & 0xF) ^ ((w >> 4) & 0x0F);
                    } else {
                        // CRC bytes: no dewhitening
                        low_nib = (payload_nibbles[i] & 0xF);
                        high_nib = (payload_nibbles[i + 1] & 0xF);
                    }
                    uint8_t byte = static_cast<uint8_t>((high_nib << 4) | low_nib);
                    unwhitened.push_back(byte);
                }
                if (unwhitened.size() > expected_payload_len + (expected_crc ? 2 : 0)) {
                    unwhitened.resize(expected_payload_len + (expected_crc ? 2 : 0));
                }
                std::cout << "Payload bytes (dewhitened):";
                for (std::size_t i = 0; i < std::min<std::size_t>(unwhitened.size(), expected_payload_len); ++i) {
                    std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                              << static_cast<int>(unwhitened[i]) << std::dec;
                }
                std::cout << "\n";
                if (expected_payload_len > 0) {
                    std::string ascii(unwhitened.begin(), unwhitened.begin() + std::min<std::size_t>(expected_payload_len, unwhitened.size()));
                    std::cout << "Payload ASCII: " << ascii << "\n";
                }
                if (!options.payload.empty() && expected_payload_len > 0) {
                    bool payload_match = true;
                    const std::string& expected_payload = options.payload;
                    if (expected_payload.size() != expected_payload_len) {
                        payload_match = false;
                        std::cout << "[payload] expected length " << expected_payload.size()
                                  << " does not match decoded length "
                                  << expected_payload_len << "\n";
                    } else if (unwhitened.size() < expected_payload_len ||
                               !std::equal(expected_payload.begin(), expected_payload.end(),
                                           unwhitened.begin())) {
                        payload_match = false;
                        std::cout << "[payload] decoded payload differs from expected string\n";
                    }
                    if (!payload_match) {
                        compare_failure = true;
                    }
                }

                if (expected_crc && unwhitened.size() >= expected_payload_len + 2) {
                    // GNU Radio CRC verification algorithm:
                    // 1. Compute CRC on first (payload_len - 2) bytes
                    // 2. XOR with last 2 payload bytes
                    // 3. Compare with received CRC (little-endian)
                    std::vector<uint8_t> crc_data(unwhitened.begin(),
                                                  unwhitened.begin() + expected_payload_len - 2);
                    uint16_t computed_crc = compute_lora_crc(crc_data);
                    // XOR with last 2 payload bytes
                    if (expected_payload_len >= 2) {
                        computed_crc ^= unwhitened[expected_payload_len - 1];  // last byte
                        computed_crc ^= static_cast<uint16_t>(unwhitened[expected_payload_len - 2]) << 8;  // second-to-last byte
                    }
                    const uint8_t crc_byte0 = unwhitened[expected_payload_len];
                    const uint8_t crc_byte1 = unwhitened[expected_payload_len + 1];
                    const uint16_t decoded_crc = static_cast<uint16_t>(crc_byte0) |
                                                 (static_cast<uint16_t>(crc_byte1) << 8);
                    const bool crc_ok = (computed_crc == decoded_crc);
                    std::cout << "[payload] CRC decoded=0x" << std::hex << std::setw(4)
                              << std::setfill('0') << decoded_crc
                              << " computed=0x" << std::setw(4) << computed_crc
                              << std::dec << (crc_ok ? " OK" : " MISMATCH") << "\n";
                }

                // Dump decoded payload bytes to file if requested
                if (options.dump_payload && expected_payload_len > 0) {
                    std::ofstream payload_file(*options.dump_payload, std::ios::binary);
                    if (payload_file) {
                        std::size_t bytes_to_write = std::min<std::size_t>(
                            unwhitened.size(), expected_payload_len);
                        payload_file.write(reinterpret_cast<const char*>(unwhitened.data()),
                                           static_cast<std::streamsize>(bytes_to_write));
                        std::cout << "[dump-payload] wrote " << bytes_to_write
                                  << " bytes to " << options.dump_payload->string() << "\n";
                    } else {
                        std::cerr << "[dump-payload] failed to open "
                                  << options.dump_payload->string() << "\n";
                    }
                }

                if (options.dump_stages && have_stage_outputs) {
                    std::filesystem::path base = *options.dump_stages;
                    if (base.extension() == ".cf32") {
                        base.replace_extension("");
                    }
                    auto dump_stage_file = [&](const char* suffix,
                                               const auto& host_vec) {
                        std::filesystem::path path = base;
                        path += suffix;
                        write_stage_file(path, host_vec);
                    };
                    dump_stage_file("_fft.txt", stage_outputs.fft);
                    dump_stage_file("_gray.txt", stage_outputs.gray);
                    dump_stage_file("_deinterleaver.txt", stage_outputs.deinterleaver);
                    dump_stage_file("_hamming.txt", stage_outputs.hamming);
                    std::cout << "Dumped stage outputs using prefix " << base.generic_string() << "\n";
                }

                if (options.compare_root && have_stage_outputs) {
                    auto stage_results = compare_with_reference(stage_outputs, *options.compare_root);
                    summary.stage_results = stage_results;
                    summary.compare_run = true;
                    const bool verbose_compare = (std::getenv("HOST_SIM_VERBOSE_COMPARE") != nullptr);
                    std::size_t stage_total = 0;
                    std::ostringstream summary_line;
                    summary_line << "[compare] summary:";
                    std::ostringstream ci_line;
                    ci_line << "[ci-summary] stages";

                    for (const auto& res : stage_results) {
                        if (verbose_compare) {
                            if (res.reference_missing) {
                                std::cout << "[compare] stage " << res.label << ": reference file missing\n";
                            } else {
                                std::cout << "[compare] stage " << res.label << ": host=" << res.host_count
                                          << " ref=" << res.ref_count << " mismatches=" << res.mismatches;
                                if (res.alignment_offset) {
                                    if (res.alignment_relative_to_reference) {
                                        std::cout << " (aligned at ref offset " << *res.alignment_offset << ")";
                                    } else {
                                        std::cout << " (aligned at host offset " << *res.alignment_offset << ")";
                                    }
                                } else {
                                    std::cout << " (no exact alignment, comparing from start)";
                                }
                                if (res.mismatches == 0) {
                                    std::cout << " (OK)\n";
                                } else if (res.first_diff_index) {
                                    std::cout << " first_diff@" << *res.first_diff_index;
                                    if (res.host_value && res.ref_value) {
                                        std::cout << " host=" << *res.host_value
                                                  << " ref=" << *res.ref_value;
                                    }
                                    std::cout << "\n";
                                } else if (res.host_count > 0 && res.ref_count == 0) {
                                    std::cout << " reference empty\n";
                                } else {
                                    std::cout << " length mismatch\n";
                                }
                            }
                        }

                        stage_total += res.mismatches;
                        if (res.mismatches > 0 || res.reference_missing) {
                            compare_failure = true;
                        }

                        summary_line << ' ' << res.label << '=' << build_stage_summary_token(res);
                        ci_line << ' ' << res.label << '=' << res.mismatches << '/' << res.host_count;
                    }

                    total_stage_mismatches += stage_total;

                    std::cout << summary_line.str() << "\n";
                    if (verbose_compare) {
                        if (stage_total == 0) {
                            std::cout << "[compare] all stage outputs match reference\n";
                        } else {
                            std::cout << "[compare] aggregate stage mismatches: " << stage_total << "\n";
                        }
                    } else if (stage_total > 0) {
                        std::cout << "[compare] stage mismatches detected: " << stage_total << "\n";
                    }
                    std::cout << ci_line.str() << "\n";
                }
                if (options.summary_output) {
                    auto instrumentation = run_scheduler_instrumentation(
                        samples,
                        *metadata,
                        alignment_samples,
                        256);
                    summary.stage_timings_ns = std::move(instrumentation.stage_timings_ns);
                    summary.memory_usage_bytes = std::move(instrumentation.symbol_memory_bytes);
                }
            } // end if (header.success)

            // --- multi-packet loop advance ---
            if (options.multi_packet) {
                // Estimate burst end: alignment + all decoded symbols
                const std::size_t symbols_consumed = symbols.size();
                const std::size_t burst_end_sample = alignment_samples +
                    symbols_consumed * static_cast<std::size_t>(sps);
                // Advance past this burst with a small gap margin
                const std::size_t next_offset = burst_end_sample + static_cast<std::size_t>(sps) * 4;
                // If we didn't advance (e.g., no symbols decoded), skip one symbol period
                if (next_offset <= multi_search_offset) {
                    multi_search_offset += static_cast<std::size_t>(sps) * 8;
                } else {
                    multi_search_offset = next_offset;
                }
                if (!header.success) {
                    std::cout << "[multi] packet #" << multi_packet_index
                              << ": header decode failed, skipping burst\n";
                }
                ++multi_packet_index;
                symbols.clear();
                continue; // next burst
            }
            break; // single-packet mode: done
          } // end multi-packet loop
            summary.stage_mismatches = total_stage_mismatches;
        }

        if (options.dump_symbols && !symbols.empty()) {
            std::ofstream sym_out(*options.dump_symbols);
            if (!sym_out) {
                throw std::runtime_error("Failed to open symbol dump file: " + options.dump_symbols->string());
            }
            for (std::size_t i = 0; i < symbols.size(); ++i) {
                sym_out << symbols[i] << '\n';
            }
            std::cout << "Dumped " << symbols.size() << " symbols to " << options.dump_symbols->generic_string() << "\n";
        }

        if (options.dump_iq) {
            std::ofstream iq_out(*options.dump_iq, std::ios::binary);
            if (!iq_out) {
                throw std::runtime_error("Failed to open IQ dump file: " + options.dump_iq->string());
            }
            const float* raw = reinterpret_cast<const float*>(&samples[alignment_samples]);
            const std::size_t float_count = (samples.size() > alignment_samples ? samples.size() - alignment_samples : 0) * 2;
            iq_out.write(reinterpret_cast<const char*>(raw), static_cast<std::streamsize>(float_count * sizeof(float)));
            std::cout << "Dumped aligned IQ to " << options.dump_iq->generic_string() << "\n";
        }
        std::vector<uint8_t> payload_bytes(options.payload.begin(), options.payload.end());
        host_sim::WhiteningSequencer sequencer;
        const auto whitened = sequencer.apply(payload_bytes);
        const auto recovered = sequencer.undo(whitened);

        const bool roundtrip_ok = (recovered == payload_bytes);

        std::cout << "Whitening preview: ";
        for (std::size_t i = 0; i < std::min<std::size_t>(whitened.size(), 8); ++i) {
            std::cout << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                      << static_cast<int>(whitened[i]) << ' ';
        }
        std::cout << std::dec << "\nRound-trip whitening " << (roundtrip_ok ? "succeeded" : "FAILED") << "\n";

        if (options.stats_output) {
            write_stats_json(*options.stats_output, stats, options);
            std::cout << "Wrote stats to " << options.stats_output->generic_string() << "\n";
        }

        summary.whitening_roundtrip_ok = roundtrip_ok;
        summary.stage_mismatches = total_stage_mismatches;
        summary.reference_mismatches = reference_mismatches;
        if (options.summary_output) {
            write_summary_json(*options.summary_output, summary);
            std::cout << "Wrote summary to " << options.summary_output->generic_string() << "\n";
        }

        if (reference_mismatches > 0) {
            compare_failure = true;
            std::cout << "[summary] reference demod mismatches observed: " << reference_mismatches << "\n";
        }
        if (total_stage_mismatches > 0) {
            compare_failure = true;
            std::cout << "[summary] stage comparison mismatches observed: " << total_stage_mismatches << "\n";
        }

        const bool success = roundtrip_ok && !compare_failure;
        return success ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
