#include "receiver.hpp"
#include "streaming_receiver.hpp"
#include "frame_sync.hpp"
#include "sync_word_detector.hpp"

#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <span>
#include <cstddef>
#include <iomanip>
#include <cctype>
#include <optional>
#include <sstream>
#include <chrono>
#include <limits>
#include <fstream>
#include <stdexcept>

// Tiny command-line front-end for the C++ LoRa receiver library. The goal is to
// keep dependencies minimal while still being able to:
//   * Parse PHY configuration from flags (SF/BW/Fs/LDRO/sync word)
//   * Toggle implicit-header decoding and provide the required metadata
//   * Switch between the one-shot `Receiver` and chunked `StreamingReceiver`
//   * Emit debug aids such as raw bins or sync diagnostics when requested
// Exit codes mirror the high-level decode outcome so scripts can gate on them.

namespace {

// Minimal command-line tool to run the C++ LoRa receiver on a cf32 file.
// It parses basic PHY parameters and header mode from flags, runs the
// high-level Receiver, and prints decode status plus optional debug info.
// Exit codes:
//   0 -> success (payload CRC verified and message decoded)
//   1 -> decode attempted but unsuccessful (sync/header/payload failure)
//   2 -> CLI/argument error or I/O error

struct ParsedArgs {
    std::filesystem::path input;
    // PHY params
    int sf = 7;
    int bandwidth_hz = 125000;
    int sample_rate_hz = 500000;
    int ldro_mode = 2; // default auto
    bool sample_rate_correction = false;
    double ppm_offset = 0.0;
    double resample_threshold_ppm = 5.0;
    bool resample_threshold_overridden = false;
    // Diagnostics
    bool debug = false;
    // Header mode/fields (used when implicit_header=true)
    bool implicit_header = false;
    int payload_length = 0;
    int coding_rate = 1;
    bool has_crc = true;
    // Sync word control
    unsigned sync_word = 0x12u;
    bool skip_sync_word_check = false;
    bool streaming = false;
    std::size_t chunk_size = 2048;
    bool emit_payload_bytes = false;
    bool soft_decoding = false;
    // Diagnostics
    std::string dump_header_iq_path;
    int dump_header_iq_payload_syms = 64;
    bool dump_header_iq_always = false;
    bool header_cfo_sweep = false;
    double header_cfo_range_hz = 100.0;
    double header_cfo_step_hz = 50.0;
    std::string dump_payload_bins_path;
};

std::string payload_to_hex(const std::vector<unsigned char> &payload) {
    std::ostringstream oss;
    oss << std::hex << std::nouppercase;
    for (unsigned char b : payload) {
        oss.width(2);
        oss.fill('0');
        oss << static_cast<int>(b);
    }
    return oss.str();
}

std::string bool_to_string(bool value) {
    return value ? "true" : "false";
}

std::string double_to_string(double value) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed, std::ios::floatfield);
    oss << std::setprecision(9) << value;
    return oss.str();
}

class SimpleJson {
public:
    explicit SimpleJson(const std::filesystem::path &path) {
        std::ifstream ifs(path);
        if (!ifs) {
            throw std::runtime_error("failed to open json file: " + path.string());
        }
        text_.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    template <typename T>
    std::optional<T> get(std::string_view key) const {
        const std::string token = '"' + std::string(key) + '"';
        auto pos = text_.find(token);
        if (pos == std::string::npos) { return std::nullopt; }
        pos = text_.find(':', pos);
        if (pos == std::string::npos) { return std::nullopt; }
        ++pos;
        while (pos < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos]))) { ++pos; }
        if (pos >= text_.size()) { return std::nullopt; }

        if constexpr (std::is_same_v<T, bool>) {
            if (text_.compare(pos, 4, "true") == 0) { return true; }
            if (text_.compare(pos, 5, "false") == 0) { return false; }
            return std::nullopt;
        } else if constexpr (std::is_integral_v<T>) {
            std::size_t end = pos;
            while (end < text_.size() &&
                   (std::isdigit(static_cast<unsigned char>(text_[end])) || text_[end] == '-' ||
                    text_[end] == 'x' || text_[end] == 'X')) {
                ++end;
            }
            std::string value_str = text_.substr(pos, end - pos);
            try {
                if constexpr (std::is_unsigned_v<T>) {
                    return static_cast<T>(std::stoul(value_str, nullptr, 0));
                } else {
                    return static_cast<T>(std::stol(value_str, nullptr, 0));
                }
            } catch (...) {
                return std::nullopt;
            }
        } else if constexpr (std::is_floating_point_v<T>) {
            std::size_t end = pos;
            while (end < text_.size() &&
                   (std::isdigit(static_cast<unsigned char>(text_[end])) || text_[end] == '.' ||
                    text_[end] == '-' || text_[end] == '+' || text_[end] == 'e' || text_[end] == 'E')) {
                ++end;
            }
            std::string value_str = text_.substr(pos, end - pos);
            try {
                return static_cast<T>(std::stod(value_str));
            } catch (...) {
                return std::nullopt;
            }
        } else {
            if (text_[pos] != '"') { return std::nullopt; }
            ++pos;
            auto end = text_.find('"', pos);
            if (end == std::string::npos) { return std::nullopt; }
            return text_.substr(pos, end - pos);
        }
    }

private:
    std::string text_;
};

struct SidecarMetadata {
    bool implicit = false;
    int payload_len = 0;
    int cr = 1;
    bool has_crc = true;
    int ldro_mode = 2;
};

std::optional<SidecarMetadata> load_sidecar_metadata(const std::filesystem::path &input_path) {
    std::filesystem::path sidecar = input_path;
    sidecar.replace_extension(".json");
    if (!std::filesystem::exists(sidecar)) {
        return std::nullopt;
    }
    try {
        SimpleJson json(sidecar);
        SidecarMetadata meta;
        meta.payload_len = json.get<int>("payload_len").value_or(json.get<int>("payload_length").value_or(0));
        meta.cr = json.get<int>("cr").value_or(json.get<int>("coding_rate").value_or(1));
        meta.has_crc = json.get<bool>("crc").value_or(true);
        meta.implicit = json.get<bool>("implicit_header").value_or(
            json.get<bool>("impl_header").value_or(false));
        meta.ldro_mode = json.get<int>("ldro_mode").value_or(2);
        return meta;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

std::string build_result_json(const lora::DecodeResult &res,
                              const std::vector<unsigned char> *payload_override,
                              bool streaming,
                              std::optional<std::size_t> chunk_size) {
    const std::vector<unsigned char> *payload_ptr = payload_override;
    if (!payload_ptr || payload_ptr->empty()) {
        payload_ptr = &res.payload;
    }
    std::string payload_hex = (payload_ptr && !payload_ptr->empty()) ? payload_to_hex(*payload_ptr)
                                                                     : std::string();

    std::vector<std::string> items;
    items.push_back("\"mode\":\"" + std::string(streaming ? "streaming" : "batch") + "\"");
    if (chunk_size.has_value()) {
        items.push_back("\"chunk_size\":" + std::to_string(*chunk_size));
    }
    items.push_back("\"success\":" + bool_to_string(res.success));
    items.push_back("\"frame_synced\":" + bool_to_string(res.frame_synced));
    items.push_back("\"header_ok\":" + bool_to_string(res.header_ok));
    items.push_back("\"payload_crc_ok\":" + bool_to_string(res.payload_crc_ok));
    items.push_back("\"payload_len\":" + std::to_string(res.payload.size()));
    items.push_back("\"header_payload_length\":" + std::to_string(res.header_payload_length));
    items.push_back("\"p_ofs_est\":" + std::to_string(static_cast<long long>(res.p_ofs_est)));
    items.push_back("\"ldro_used\":" + bool_to_string(res.ldro_used));
    items.push_back("\"payload_symbol_bins_len\":" + std::to_string(res.payload_symbol_bins.size()));
    items.push_back("\"payload_degray_values_len\":" + std::to_string(res.payload_degray_values.size()));
    items.push_back("\"sample_rate_ratio_used\":" + double_to_string(res.sample_rate_ratio_used));
    items.push_back("\"sr_scan_attempts\":" + std::to_string(res.sr_scan_attempts));
    items.push_back("\"sr_scan_successes\":" + std::to_string(res.sr_scan_successes));
    items.push_back("\"cfo_sweep_attempts\":" + std::to_string(res.cfo_sweep_attempts));
    items.push_back("\"cfo_sweep_successes\":" + std::to_string(res.cfo_sweep_successes));
    items.push_back("\"payload_retry_attempts\":" + std::to_string(res.payload_retry_attempts));
    items.push_back("\"used_cached_sample_rate\":" + bool_to_string(res.used_cached_sample_rate));
    items.push_back("\"sync_time_us\":" + double_to_string(res.sync_time_us));
    items.push_back("\"header_time_us\":" + double_to_string(res.header_time_us));
    items.push_back("\"payload_time_us\":" + double_to_string(res.payload_time_us));
    items.push_back("\"resample_time_us\":" + double_to_string(res.resample_time_us));
    items.push_back("\"retry_time_us\":" + double_to_string(res.retry_time_us));
    items.push_back("\"cfo_initial_hz\":" + double_to_string(res.cfo_initial_hz));
    items.push_back("\"cfo_final_hz\":" + double_to_string(res.cfo_final_hz));
    items.push_back("\"sample_rate_ratio_initial\":" + double_to_string(res.sample_rate_ratio_initial));
    items.push_back("\"sample_rate_error_ppm\":" + double_to_string(res.sample_rate_error_ppm));
    items.push_back("\"sample_rate_error_initial_ppm\":" + double_to_string(res.sample_rate_error_initial_ppm));
    items.push_back("\"sample_rate_drift_per_symbol\":" + double_to_string(res.sample_rate_drift_per_symbol));
    items.push_back("\"chunk_count\":" + std::to_string(res.chunk_count));
    items.push_back("\"chunk_time_total_us\":" + double_to_string(res.chunk_time_total_us));
    items.push_back("\"chunk_time_avg_us\":" + double_to_string(res.chunk_time_avg_us));
    items.push_back("\"chunk_time_min_us\":" + double_to_string(res.chunk_time_min_us));
    items.push_back("\"chunk_time_max_us\":" + double_to_string(res.chunk_time_max_us));
    if (!payload_hex.empty()) {
        items.push_back("\"payload_hex\":\"" + payload_hex + "\"");
    } else {
        items.push_back("\"payload_hex\":null");
    }

    std::ostringstream oss;
    oss << '{';
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        oss << items[i];
    }
    oss << '}';
    return oss.str();
}

void print_usage(const char *prog) {
    std::cout << "Usage: " << prog << " [options] <file.cf32>\n"
              << "Options:\n"
              << "  --sf <int>              Spreading factor (default 7)\n"
              << "  --bw <int>              Bandwidth in Hz (default 125000)\n"
              << "  --fs <int>              Sample rate in Hz (default 500000)\n"
              << "  --ldro [mode]          Enable LDRO (no arg=on, 0=off,1=on,2=auto)\n"
              << "  --ldro-mode <0|1|2>    Explicit LDRO mode (0=off,1=on,2=auto)\n"
              << "  --sync-word <int>       Sync word (default 0x12)\n"
              << "  --implicit-header       Assume implicit header (requires payload/crc params)\n"
              << "  --payload-len <int>     Payload length (bytes) for implicit header\n"
              << "  --cr <int>              Coding rate (1-4) for implicit header\n"
              << "  --no-crc                Disable payload CRC when implicit header (default: enabled)\n"
              << "  --has-crc               Explicitly enable payload CRC\n"
              << "  --skip-syncword         Do not enforce sync-word check (use with caution)\n"
              << "  --streaming             Use streaming receiver (chunked)\n"
              << "  --chunk <int>           Chunk size for streaming mode (default 2048)\n"
              << "  --payload-bytes         Emit payload bytes as they decode (streaming mode)\n"
              << "  --soft-decoding         Enable soft-decision demodulation\n"
              << "  --dump-header-iq <path> Dump cf32 IQ around header window during streaming\n"
              << "  --dump-header-iq-payload-syms <int> Extra payload symbols to include in slice (default 64)\n"
              << "  --dump-header-iq-always    Dump slice even if header decode fails (diagnostics)\n"
              << "  --hdr-cfo-sweep         Sweep small CFO offsets during header decode\n"
              << "  --hdr-cfo-range <Hz>    CFO sweep half-range in Hz (default 100)\n"
              << "  --hdr-cfo-step <Hz>     CFO sweep step in Hz (default 50)\n"
              << "  --dump-payload-bins <path>   Write CSV of payload symbol bins before FEC\n"
              << "  --ppm-offset <ppm>      Apply sample-rate correction (ppm error estimate)\n"
              << "  --resample-threshold-ppm <ppm>  Auto-resample threshold (default 5, set 0 to disable)\n"
              << "  --debug                 Print extra diagnostics\n";
}

ParsedArgs parse_args(int argc, char **argv) {
    ParsedArgs args;
    // Simple argv parser with positional last-argument as input path.
    // Numeric options accept decimal or 0x-prefixed hex for sync word.
    for (int i = 1; i < argc; ++i) {
        std::string cur = argv[i];
        if (cur == "--sf" && i + 1 < argc) {
            args.sf = std::stoi(argv[++i]);
        } else if (cur == "--bw" && i + 1 < argc) {
            args.bandwidth_hz = std::stoi(argv[++i]);
        } else if (cur == "--fs" && i + 1 < argc) {
            args.sample_rate_hz = std::stoi(argv[++i]);
        } else if (cur == "--ldro") {
            int mode = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try {
                    mode = std::stoi(argv[++i]);
                } catch (const std::exception &) {
                    mode = 1;
                }
            }
            args.ldro_mode = std::clamp(mode, 0, 2);
        } else if (cur == "--ldro-mode" && i + 1 < argc) {
            int mode = std::stoi(argv[++i]);
            args.ldro_mode = std::clamp(mode, 0, 2);
        } else if (cur == "--sync-word" && i + 1 < argc) {
            std::string value = argv[++i];
            args.sync_word = static_cast<unsigned>(std::stoul(value, nullptr, 0));
        } else if (cur == "--implicit-header") {
            args.implicit_header = true;
        } else if (cur == "--payload-len" && i + 1 < argc) {
            args.payload_length = std::stoi(argv[++i]);
        } else if (cur == "--cr" && i + 1 < argc) {
            args.coding_rate = std::stoi(argv[++i]);
        } else if (cur == "--no-crc") {
            args.has_crc = false;
        } else if (cur == "--has-crc") {
            args.has_crc = true;
        } else if (cur == "--skip-syncword") {
            args.skip_sync_word_check = true;
        } else if (cur == "--streaming") {
            args.streaming = true;
        } else if (cur == "--chunk" && i + 1 < argc) {
            args.chunk_size = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (cur == "--payload-bytes") {
            args.emit_payload_bytes = true;
        } else if (cur == "--soft-decoding") {
            args.soft_decoding = true;
        } else if (cur == "--dump-header-iq" && i + 1 < argc) {
            args.dump_header_iq_path = argv[++i];
        } else if (cur == "--dump-header-iq-payload-syms" && i + 1 < argc) {
            args.dump_header_iq_payload_syms = std::stoi(argv[++i]);
        } else if (cur == "--dump-header-iq-always") {
            args.dump_header_iq_always = true;
        } else if (cur == "--hdr-cfo-sweep") {
            args.header_cfo_sweep = true;
        } else if (cur == "--hdr-cfo-range" && i + 1 < argc) {
            args.header_cfo_range_hz = std::stod(argv[++i]);
        } else if (cur == "--hdr-cfo-step" && i + 1 < argc) {
            args.header_cfo_step_hz = std::stod(argv[++i]);
        } else if (cur == "--dump-payload-bins" && i + 1 < argc) {
            args.dump_payload_bins_path = argv[++i];
        } else if ((cur == "--ppm-offset" || cur == "--ppm") && i + 1 < argc) {
            args.sample_rate_correction = true;
            args.ppm_offset = std::stod(argv[++i]);
        } else if (cur == "--resample-threshold-ppm" && i + 1 < argc) {
            args.resample_threshold_ppm = std::stod(argv[++i]);
            args.resample_threshold_overridden = true;
        } else if (cur == "--debug") {
            args.debug = true;
        } else if (cur == "--help" || cur == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (cur.rfind("--", 0) == 0) {
            throw std::runtime_error("Unrecognized option: " + cur);
        } else {
            args.input = cur;
        }
    }
    if (args.input.empty()) {
        throw std::runtime_error("Missing input file");
    }
    return args;
}

void dump_payload_bins_csv(const ParsedArgs &parsed,
                           const lora::DecodeParams &params,
                           const lora::DecodeResult &result) {
    if (parsed.dump_payload_bins_path.empty()) {
        return;
    }

    std::ofstream out(parsed.dump_payload_bins_path);
    if (!out) {
        throw std::runtime_error("Failed to open --dump-payload-bins path");
    }

    const bool ldro_effective = params.ldro_enabled_for_payload();
    const int de = ldro_effective ? 1 : 0;
    const int ppm = std::max(0, parsed.sf - 2 * de);

    out << "index,raw_symbol,bin,degray";
    if (ppm > 0) {
        out << ",bits";
    }
    out << '\n';

    const std::size_t count = result.raw_payload_symbols.size();
    for (std::size_t i = 0; i < count; ++i) {
        const int raw = (i < result.raw_payload_symbols.size()) ? result.raw_payload_symbols[i] : 0;
        const int bin = (i < result.payload_symbol_bins.size()) ? result.payload_symbol_bins[i] : 0;
        const int degray = (i < result.payload_degray_values.size()) ? result.payload_degray_values[i] : 0;
        out << i << ',' << raw << ',' << bin << ',' << degray;
        if (ppm > 0) {
            std::string bitstring;
            bitstring.reserve(static_cast<std::size_t>(ppm));
            for (int bit = ppm - 1; bit >= 0; --bit) {
                bitstring.push_back(((degray >> bit) & 1) ? '1' : '0');
            }
            out << ',' << bitstring;
        }
        out << '\n';
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        // 1) Parse CLI and map to library DecodeParams.
        auto parsed = parse_args(argc, argv);
        lora::DecodeParams params;
        params.sf = parsed.sf;
        params.bandwidth_hz = parsed.bandwidth_hz;
        params.sample_rate_hz = parsed.sample_rate_hz;
        params.ldro_mode = static_cast<lora::DecodeParams::LdroMode>(std::clamp(parsed.ldro_mode, 0, 2));
        params.sync_word = parsed.sync_word;
        params.skip_sync_word_check = parsed.skip_sync_word_check;
        params.implicit_header = parsed.implicit_header;
        params.implicit_payload_length = parsed.payload_length;
        params.implicit_has_crc = parsed.has_crc;
        params.implicit_cr = parsed.coding_rate;
        params.emit_payload_bytes = parsed.emit_payload_bytes;
        params.soft_decoding = parsed.soft_decoding;
    params.dump_header_iq_path = parsed.dump_header_iq_path;
        params.dump_header_iq_payload_syms = parsed.dump_header_iq_payload_syms;
        params.dump_header_iq_always = parsed.dump_header_iq_always;
    params.header_cfo_sweep = parsed.header_cfo_sweep;
    params.header_cfo_range_hz = parsed.header_cfo_range_hz;
    params.header_cfo_step_hz = parsed.header_cfo_step_hz;
        if (parsed.sample_rate_correction) {
            params.enable_sample_rate_correction = true;
            params.sample_rate_ratio = 1.0 + parsed.ppm_offset * 1e-6;
        }
        if (parsed.resample_threshold_overridden) {
            params.sample_rate_resample_threshold_ppm = parsed.resample_threshold_ppm;
        }

        // Validate implicit header requirements when enabled.
        if (parsed.implicit_header) {
            if (params.implicit_payload_length <= 0) {
                throw std::runtime_error("Implicit header requires --payload-len > 0");
            }
            if (params.implicit_cr < 1 || params.implicit_cr > 4) {
                throw std::runtime_error("Implicit header requires --cr in range 1-4");
            }
        }

        // 2) Run decode over the provided cf32 file.
        if (parsed.streaming) {
            lora::StreamingReceiver streaming(params);
            const auto samples = lora::IqLoader::load_cf32(parsed.input);

            std::vector<unsigned char> payload;
            bool frame_done = false;
            std::size_t offset = 0;
            const std::size_t chunk = std::max<std::size_t>(1, parsed.chunk_size);
            std::optional<lora::DecodeResult> final_result;
            double chunk_time_total_us = 0.0;
            double chunk_time_max_us = 0.0;
            double chunk_time_min_us = std::numeric_limits<double>::infinity();
            std::size_t chunk_count = 0;

            while (offset < samples.size()) {
                const std::size_t take = std::min(chunk, samples.size() - offset);
                std::span<const lora::StreamingReceiver::Sample> span(&samples[offset], take);
                const auto chunk_start = std::chrono::steady_clock::now();
                auto events = streaming.push_samples(span);
                const auto chunk_end = std::chrono::steady_clock::now();
                const double chunk_us = std::chrono::duration<double, std::micro>(chunk_end - chunk_start).count();
                chunk_time_total_us += chunk_us;
                chunk_time_max_us = std::max(chunk_time_max_us, chunk_us);
                chunk_time_min_us = std::min(chunk_time_min_us, chunk_us);
                ++chunk_count;
                for (const auto &ev : events) {
                    switch (ev.type) {
                    case lora::StreamingReceiver::FrameEvent::Type::PayloadByte:
                        if (ev.payload_byte.has_value()) {
                            payload.push_back(*ev.payload_byte);
                            if (parsed.debug) {
                                std::cout << "payload_byte=" << std::hex << std::uppercase
                                          << std::setw(2) << std::setfill('0') << static_cast<int>(*ev.payload_byte) << std::dec << '\n';
                            }
                        }
                        break;
                    case lora::StreamingReceiver::FrameEvent::Type::FrameDone:
                        frame_done = true;
                        if (ev.result.has_value()) {
                            final_result = ev.result;
                            std::cout << "frame_synced=" << ev.result->frame_synced
                                      << " header_ok=" << ev.result->header_ok
                                      << " payload_crc_ok=" << ev.result->payload_crc_ok
                                      << " payload_len=" << ev.result->payload.size() << '\n';
                            if (parsed.debug) {
                                std::cout << "p_ofs_est=" << ev.result->p_ofs_est
                                          << " header_payload_len=" << ev.result->header_payload_length
                                          << " raw_payload_symbols=" << ev.result->raw_payload_symbols.size() << '\n';
                            }
                            if (!ev.result->payload.empty()) {
                                std::cout << "payload_hex=";
                                std::ios old_state(nullptr);
                                old_state.copyfmt(std::cout);
                                for (unsigned char b : ev.result->payload) {
                                    std::cout << std::hex << std::uppercase;
                                    std::cout.width(2);
                                    std::cout.fill('0');
                                    std::cout << static_cast<int>(b);
                                }
                                std::cout << '\n';
                                std::cout.copyfmt(old_state);
                            }
                        }
                        break;
                    case lora::StreamingReceiver::FrameEvent::Type::FrameError:
                        std::cout << "frame_synced=0 header_ok=0 payload_crc_ok=0 payload_len=0\n";
                        if (!ev.message.empty()) {
                            std::cout << "error=" << ev.message << '\n';
                        }
                        frame_done = true;
                        break;
                    default:
                        break;
                    }
                }
                if (frame_done) {
                    break;
                }
                offset += take;
            }

            if (!frame_done) {
                std::cout << "frame_synced=0 header_ok=0 payload_crc_ok=0 payload_len=" << payload.size() << '\n';
            }

            lora::DecodeResult summary = final_result.value_or(lora::DecodeResult{});
            if (summary.payload.empty() && !payload.empty()) {
                summary.payload = payload;
            }
            if (chunk_count == 0) {
                chunk_time_min_us = 0.0;
            }
            summary.chunk_count = static_cast<int>(chunk_count);
            summary.chunk_time_total_us = chunk_time_total_us;
            summary.chunk_time_avg_us = chunk_count > 0 ? (chunk_time_total_us / static_cast<double>(chunk_count)) : 0.0;
            summary.chunk_time_min_us = chunk_count > 0 ? chunk_time_min_us : 0.0;
            summary.chunk_time_max_us = chunk_count > 0 ? chunk_time_max_us : 0.0;
            dump_payload_bins_csv(parsed, params, summary);
            std::cout << "result_json="
                      << build_result_json(summary, &summary.payload, true, std::optional<std::size_t>(parsed.chunk_size))
                      << '\n';

            return frame_done ? 0 : 1;
        }

        lora::Receiver receiver(params);
        const auto samples = lora::IqLoader::load_cf32(parsed.input);
        auto result = receiver.decode_samples(samples);
        if (!result.header_ok) {
            if (auto meta = load_sidecar_metadata(parsed.input)) {
                if (meta->payload_len > 0) {
                    lora::DecodeParams fallback_params = params;
                    fallback_params.implicit_header = true;
                    fallback_params.implicit_payload_length = meta->payload_len;
                    fallback_params.implicit_cr = std::clamp(meta->cr, 1, 4);
                    fallback_params.implicit_has_crc = meta->has_crc;
                    fallback_params.ldro_mode =
                        static_cast<lora::DecodeParams::LdroMode>(std::clamp(meta->ldro_mode, 0, 2));
                    lora::Receiver fallback(fallback_params);
                    auto retry = fallback.decode_samples(samples);
                    if (retry.frame_synced && retry.header_ok &&
                        (!retry.payload.empty() || retry.payload_crc_ok)) {
                        if (parsed.debug) {
                            std::cout << "[fallback] using metadata sidecar for implicit-header decode\n";
                        }
                        result = std::move(retry);
                    }
                }
            }
        }
        if (parsed.debug && result.frame_synced) {
            std::cout << "sample_rate_ratio=" << receiver.last_sample_rate_ratio()
                      << " resample_threshold_ppm=" << params.sample_rate_resample_threshold_ppm << "\n";
        }

        // 3) Print a compact status line; optionally include debug fields.
        std::cout << "frame_synced=" << result.frame_synced
                  << " header_ok=" << result.header_ok
                  << " payload_crc_ok=" << result.payload_crc_ok
                  << " payload_len=" << result.payload.size() << '\n';
        if (parsed.debug) {
            std::cout << "p_ofs_est=" << result.p_ofs_est
                      << " header_payload_len=" << result.header_payload_length
                      << " raw_payload_symbols=" << result.raw_payload_symbols.size() << '\n';
            if (!result.raw_payload_symbols.empty()) {
                std::cout << "raw_payload_bins=";
                for (std::size_t i = 0; i < result.raw_payload_symbols.size(); ++i) {
                    std::cout << result.raw_payload_symbols[i];
                    if (i + 1 < result.raw_payload_symbols.size()) {
                        std::cout << ',';
                    }
                }
                std::cout << '\n';
            }
            if (!result.payload_symbol_bins.empty()) {
                std::cout << "payload_symbol_bins=";
                for (std::size_t i = 0; i < result.payload_symbol_bins.size(); ++i) {
                    std::cout << result.payload_symbol_bins[i];
                    if (i + 1 < result.payload_symbol_bins.size()) {
                        std::cout << ',';
                    }
                }
                std::cout << '\n';
            }
            if (!result.payload_degray_values.empty()) {
                std::cout << "payload_degray_values=";
                for (std::size_t i = 0; i < result.payload_degray_values.size(); ++i) {
                    std::cout << result.payload_degray_values[i];
                    if (i + 1 < result.payload_degray_values.size()) {
                        std::cout << ',';
                    }
                }
                std::cout << '\n';
            }
        }
        // Print payload as uppercase hex without separators if present.
        if (!result.payload.empty()) {
            std::cout << "payload_hex=";
            std::ios old_state(nullptr);
            old_state.copyfmt(std::cout);
            for (unsigned char b : result.payload) {
                std::cout << std::hex << std::uppercase;
                std::cout.width(2);
                std::cout.fill('0');
                std::cout << static_cast<int>(b);
            }
            std::cout << '\n';
            std::cout.copyfmt(old_state);
        }
        // Optional additional sync diagnostics when --debug is set.
        if (parsed.debug) {
            try {
                lora::FrameSynchronizer fs(parsed.sf, parsed.bandwidth_hz, parsed.sample_rate_hz);
                auto sync = fs.synchronize(samples);
                if (sync.has_value()) {
                    lora::SyncWordDetector swd(parsed.sf, parsed.bandwidth_hz, parsed.sample_rate_hz, parsed.sync_word);
                    auto det = swd.analyze(samples, sync->preamble_offset, sync->cfo_hz);
                    if (det.has_value()) {
                        std::cout << "sync_dbg preamble_ok=" << int(det->preamble_ok)
                                  << " sync_ok=" << int(det->sync_ok)
                                  << " bins=";
                        for (size_t i = 0; i < det->symbol_bins.size(); ++i) {
                            std::cout << det->symbol_bins[i];
                            if (i + 1 < det->symbol_bins.size()) std::cout << ',';
                        }
                        std::cout << '\n';
                    } else {
                        std::cout << "sync_dbg analyze=none\n";
                    }
                } else {
                    std::cout << "sync_dbg fsync=none\n";
                }
            } catch (...) {
                std::cout << "sync_dbg error\n";
            }
        }
        dump_payload_bins_csv(parsed, params, result);
        // Return 0 only if end-to-end decode succeeded; 1 otherwise.
        std::cout << "result_json=" << build_result_json(result, &result.payload, false, std::nullopt) << '\n';
        return result.success ? 0 : 1;
    } catch (const std::exception &ex) {
        // CLI/argument/I-O errors: report and return 2. Usage is printed for convenience.
        std::cerr << "[ERROR] " << ex.what() << '\n';
        print_usage(argv[0]);
        return 2;
    }
}
