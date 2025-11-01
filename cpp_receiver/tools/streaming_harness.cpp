#include "iq_loader.hpp"
#include "streaming_receiver.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fstream>
#include <cctype>
#include <cstdlib>

// This executable is a high-level harness around the streaming LoRa PHY receiver.
// It loads one or more IQ captures, synthesizes configurable idle gaps between
// them, and feeds each vector to `lora::StreamingReceiver` in bounded chunks.
// Along the way it collects statistics, mirrors the event stream (bytes, frame
// outcomes), and prints a concise per-frame summary followed by a global tally.
// The intent is to stress-test the streaming path (buffer growth, symbol flush,
// LDRO handling, metadata fallbacks) without depending on any external systems.
//
// Typical usage:
//   streaming_harness --chunk 1024 --gap-symbols 12 path/to/frame1.cf32 path/to/frame2.cf32
// Optional per-vector metadata is discovered via a sibling .json file; missing
// fields fall back to CLI defaults (sf/bw/fs/cr/ldro/sync-word). The harness
// exits with code 0 if all frames succeed and 1 if any frame fails.

namespace {

// Minimal JSON loader for simple key-value metadata
// Minimal JSON loader for simple key-value metadata. This is intentionally
// tiny to avoid bringing in a JSON dependency for the harness.
class SimpleJson {
public:
    explicit SimpleJson(const std::filesystem::path &path) {
        std::ifstream ifs(path);
        if (!ifs) {
            throw std::runtime_error("failed to open json file: " + path.string());
        }
        m_text.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    template <typename T>
    std::optional<T> get(std::string_view key) const {
        const std::string token = '"' + std::string(key) + '"';
        auto pos = m_text.find(token);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        pos = m_text.find(':', pos);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        ++pos;
        while (pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[pos]))) {
            ++pos;
        }
        if (pos >= m_text.size()) {
            return std::nullopt;
        }
        if constexpr (std::is_same_v<T, bool>) {
            if (m_text.compare(pos, 4, "true") == 0) {
                return true;
            }
            if (m_text.compare(pos, 5, "false") == 0) {
                return false;
            }
            return std::nullopt;
        } else if constexpr (std::is_integral_v<T>) {
            std::size_t end = pos;
            while (end < m_text.size() && (std::isdigit(static_cast<unsigned char>(m_text[end])) || m_text[end] == 'x' || m_text[end] == 'X' || m_text[end] == '-')) {
                ++end;
            }
            auto value_str = m_text.substr(pos, end - pos);
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
            while (end < m_text.size() &&
                   (std::isdigit(static_cast<unsigned char>(m_text[end])) || m_text[end] == '.' ||
                    m_text[end] == '-' || m_text[end] == '+' || m_text[end] == 'e' || m_text[end] == 'E')) {
                ++end;
            }
            auto value_str = m_text.substr(pos, end - pos);
            try {
                return static_cast<T>(std::stod(value_str));
            } catch (...) {
                return std::nullopt;
            }
        } else {
            if (m_text[pos] != '"') {
                return std::nullopt;
            }
            ++pos;
            auto end = m_text.find('"', pos);
            if (end == std::string::npos) {
                return std::nullopt;
            }
            return m_text.substr(pos, end - pos);
        }
    }

private:
    std::string m_text;
};

constexpr lora::DecodeParams::LdroMode ldro_mode_from_int(int value) {
    switch (value) {
    case 1:
        return lora::DecodeParams::LdroMode::On;
    case 2:
        return lora::DecodeParams::LdroMode::Auto;
    default:
        return lora::DecodeParams::LdroMode::Off;
    }
}

constexpr int ldro_mode_to_int(lora::DecodeParams::LdroMode mode) {
    switch (mode) {
    case lora::DecodeParams::LdroMode::On:
        return 1;
    case lora::DecodeParams::LdroMode::Auto:
        return 2;
    case lora::DecodeParams::LdroMode::Off:
    default:
        return 0;
    }
}

// CLI options with reasonable defaults for common LoRa settings.
struct Args {
    int fallback_sf = 7;
    int fallback_bw = 125000;
    int fallback_fs = 500000;
    int fallback_cr = 1;
    std::optional<lora::DecodeParams::LdroMode> fallback_ldro_mode;
    unsigned fallback_sync = 0x12u;
    bool emit_bytes = false;
    std::size_t chunk = 2048;
    std::size_t gap_symbols = 8;
    std::size_t throttle_us = 0;
    std::optional<double> ppm_offset;
    std::optional<std::filesystem::path> csv_path;
    std::vector<std::filesystem::path> inputs;
};

// Print brief usage and available options.
void print_usage(const char *prog) {
    std::cout << "Usage: " << prog << " [options] <vector1.cf32> <vector2.cf32> ...\n"
              << "Options:\n"
              << "  --sf <int>            Fallback spreading factor\n"
              << "  --bw <int>            Fallback bandwidth Hz\n"
              << "  --fs <int>            Fallback sample rate Hz\n"
              << "  --cr <int>            Fallback coding rate (1-4)\n"
              << "  --ldro               Force LDRO on for captures without metadata\n"
              << "  --ldro-mode <int>    Fallback LDRO mode (0=off,1=on,2=auto)\n"
              << "  --sync-word <hex>    Fallback sync word (default 0x12)\n"
              << "  --emit-bytes         Emit payload byte events\n"
              << "  --chunk <int>        Chunk size (default 2048 samples)\n"
              << "  --gap-symbols <int>  Idle symbols between frames (default 8)\n"
              << "  --throttle-us <int>  Sleep duration between chunks (microseconds)\n"
              << "  --ppm-offset <ppm>   Apply sample-rate correction hint (ppm)\n"
              << "  --csv <path>         Append per-frame metrics to CSV file\n";
}

Args parse_args(int argc, char **argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string cur = argv[i];
        if (cur == "--sf" && i + 1 < argc) {
            args.fallback_sf = std::stoi(argv[++i]);
        } else if (cur == "--bw" && i + 1 < argc) {
            args.fallback_bw = std::stoi(argv[++i]);
        } else if (cur == "--fs" && i + 1 < argc) {
            args.fallback_fs = std::stoi(argv[++i]);
        } else if (cur == "--cr" && i + 1 < argc) {
            args.fallback_cr = std::clamp(std::stoi(argv[++i]), 1, 4);
        } else if (cur == "--ldro") {
            args.fallback_ldro_mode = lora::DecodeParams::LdroMode::On;
        } else if (cur == "--ldro-mode" && i + 1 < argc) {
            const int mode = std::stoi(argv[++i]);
            args.fallback_ldro_mode = ldro_mode_from_int(mode);
        } else if (cur == "--sync-word" && i + 1 < argc) {
            args.fallback_sync = static_cast<unsigned>(std::stoul(argv[++i], nullptr, 0));
        } else if (cur == "--emit-bytes") {
            args.emit_bytes = true;
        } else if (cur == "--chunk" && i + 1 < argc) {
            args.chunk = std::max<std::size_t>(1, std::stoul(argv[++i]));
        } else if (cur == "--throttle-us" && i + 1 < argc) {
            args.throttle_us = std::stoul(argv[++i]);
        } else if (cur == "--gap-symbols" && i + 1 < argc) {
            args.gap_symbols = std::stoul(argv[++i]);
        } else if (cur == "--ppm-offset" && i + 1 < argc) {
            args.ppm_offset = std::stod(argv[++i]);
        } else if (cur == "--csv" && i + 1 < argc) {
            args.csv_path = std::filesystem::path(argv[++i]);
        } else if (cur == "--help" || cur == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (!cur.empty() && cur.front() == '-') {
            throw std::runtime_error("unrecognized option: " + cur);
        } else {
            args.inputs.emplace_back(cur);
        }
    }
    if (args.inputs.empty()) {
        throw std::runtime_error("no input vectors provided");
    }
    return args;
}

// Metadata required to configure a frame decode, derived from JSON sidecar
// if present, otherwise using CLI fallback defaults.
struct FrameMeta {
    std::filesystem::path path;
    int sf;
    int bw;
    int fs;
    int cr;
    lora::DecodeParams::LdroMode ldro_mode;
    bool implicit;
    bool crc;
    unsigned sync;
    std::string payload_hex;
    std::optional<double> sampling_offset_ppm;
};

// Attempt to read a .json sidecar next to the .cf32; fallback to CLI defaults
// for any missing fields. This allows heterogeneous vectors in a single run.
FrameMeta load_metadata(const std::filesystem::path &path, const Args &defaults) {
    const auto fallback_ldro_mode = defaults.fallback_ldro_mode.value_or(lora::DecodeParams::LdroMode::Auto);
    FrameMeta meta{path,
                   defaults.fallback_sf,
                   defaults.fallback_bw,
                   defaults.fallback_fs,
                   defaults.fallback_cr,
                   fallback_ldro_mode,
                   false,
                   true,
                   defaults.fallback_sync,
                   {},
                   std::nullopt};
    auto json_path = path;
    json_path.replace_extension(".json");
    if (std::filesystem::exists(json_path)) {
        SimpleJson json(json_path);
        if (auto sf_opt = json.get<int>("sf")) meta.sf = *sf_opt;
        if (auto bw_opt = json.get<int>("bw")) meta.bw = *bw_opt;
        if (auto fs_opt = json.get<int>("sample_rate")) meta.fs = *fs_opt;
        if (auto fs_opt = json.get<int>("samp_rate")) meta.fs = *fs_opt;
        if (auto cr_opt = json.get<int>("cr")) meta.cr = *cr_opt;
        if (auto ldro_mode_int = json.get<int>("ldro_mode")) {
            meta.ldro_mode = ldro_mode_from_int(*ldro_mode_int);
        } else if (auto ldro_mode_bool = json.get<bool>("ldro_mode")) {
            meta.ldro_mode = *ldro_mode_bool ? lora::DecodeParams::LdroMode::On : lora::DecodeParams::LdroMode::Off;
        } else if (auto ldro_int = json.get<int>("ldro")) {
            meta.ldro_mode = ldro_mode_from_int(*ldro_int);
        } else if (auto ldro_bool = json.get<bool>("ldro")) {
            meta.ldro_mode = *ldro_bool ? lora::DecodeParams::LdroMode::On : lora::DecodeParams::LdroMode::Off;
        }
        if (auto impl_opt = json.get<bool>("impl_header")) meta.implicit = *impl_opt;
        if (auto impl_opt = json.get<bool>("implicit_header")) meta.implicit = *impl_opt;
        if (auto crc_opt = json.get<bool>("crc")) meta.crc = *crc_opt;
        if (auto sync_opt = json.get<unsigned>("sync_word")) meta.sync = *sync_opt;
        if (auto payload_hex_opt = json.get<std::string>("payload_hex")) meta.payload_hex = *payload_hex_opt;
        if (auto ppm_opt = json.get<double>("sampling_offset_ppm")) meta.sampling_offset_ppm = *ppm_opt;
        if (auto ppm_opt = json.get<double>("ppm_offset")) meta.sampling_offset_ppm = *ppm_opt;
    }
    return meta;
}

// Load an IQ vector from disk in complex float32 format.
std::vector<lora::IqLoader::Sample> load_vector(const std::filesystem::path &path) {
    return lora::IqLoader::load_cf32(path);
}

// Outcome summary for a single frame used for final reporting.
struct FrameSummary {
    std::string name;
    bool success = false;
    std::size_t payload_len = 0;
    std::size_t bytes_emitted = 0;
    std::size_t chunk_count = 0;
    double total_push_us = 0.0;
    double sample_rate_ratio_hint = 1.0;
    bool sample_rate_hint_enabled = false;
    double sample_rate_ratio_used = 1.0;
    int sr_scan_attempts = 0;
    int sr_scan_successes = 0;
    int cfo_sweep_attempts = 0;
    int cfo_sweep_successes = 0;
    int payload_retry_attempts = 0;
    bool used_cached_sample_rate = false;
    double sync_time_us = 0.0;
    double header_time_us = 0.0;
    double payload_time_us = 0.0;
    double resample_time_us = 0.0;
    double retry_time_us = 0.0;
};

// Decode a single frame with optional leading idle samples. Feeds samples in
// fixed-size chunks into the streaming receiver and aggregates emitted events.
std::pair<bool, FrameSummary> run_frame(const FrameMeta &meta,
                                        const Args &args,
                                        const std::vector<lora::IqLoader::Sample> &samples,
                                        std::size_t gap_samples_before) {
    lora::DecodeParams params;
    params.sf = meta.sf;
    params.bandwidth_hz = meta.bw;
    params.sample_rate_hz = meta.fs;
    params.ldro_mode = meta.ldro_mode;
    params.sync_word = meta.sync;
    params.skip_sync_word_check = false;
    params.implicit_header = meta.implicit;
    params.implicit_payload_length = 0;
    params.implicit_has_crc = meta.crc;
    params.implicit_cr = meta.cr;
    params.emit_payload_bytes = args.emit_bytes;
    if (args.ppm_offset.has_value() || meta.sampling_offset_ppm.has_value()) {
        const double ppm = args.ppm_offset.has_value() ? *args.ppm_offset : *meta.sampling_offset_ppm;
        params.enable_sample_rate_correction = true;
        params.sample_rate_ratio = 1.0 + ppm * 1e-6;
    }

    lora::StreamingReceiver receiver(params);

    const std::size_t chunk = args.chunk;
    FrameSummary summary{};
    summary.name = meta.path.filename().string();
    summary.sample_rate_ratio_hint = params.sample_rate_ratio;
    summary.sample_rate_hint_enabled = params.enable_sample_rate_correction;
    summary.sample_rate_ratio_used = params.sample_rate_ratio;
    std::vector<lora::IqLoader::Sample> zero_buffer(args.chunk, lora::IqLoader::Sample{0.0f, 0.0f});

    std::size_t bytes_emitted = 0;
    bool frame_done = false;
    bool frame_error = false;

    // Helper: push a chunk into the receiver and harvest events.
    auto feed_chunk = [&](std::span<const lora::IqLoader::Sample> span) {
        const auto start = std::chrono::steady_clock::now();
        auto events = receiver.push_samples(span);
        const auto end = std::chrono::steady_clock::now();
        const double us = std::chrono::duration<double, std::micro>(end - start).count();
        summary.total_push_us += us;
        ++summary.chunk_count;
        for (const auto &ev : events) {
            switch (ev.type) {
            case lora::StreamingReceiver::FrameEvent::Type::PayloadByte:
                if (ev.payload_byte.has_value()) {
                    ++bytes_emitted;
                }
                break;
            case lora::StreamingReceiver::FrameEvent::Type::FrameDone:
                frame_done = true;
                summary.payload_len = ev.result ? ev.result->payload.size() : 0;
                summary.success = ev.result ? ev.result->success : false;
                if (ev.result.has_value()) {
                    summary.sample_rate_ratio_used = ev.result->sample_rate_ratio_used;
                    summary.sr_scan_attempts = ev.result->sr_scan_attempts;
                    summary.sr_scan_successes = ev.result->sr_scan_successes;
                    summary.cfo_sweep_attempts = ev.result->cfo_sweep_attempts;
                    summary.cfo_sweep_successes = ev.result->cfo_sweep_successes;
                    summary.payload_retry_attempts = ev.result->payload_retry_attempts;
                    summary.used_cached_sample_rate = ev.result->used_cached_sample_rate;
                    summary.sync_time_us = ev.result->sync_time_us;
                    summary.header_time_us = ev.result->header_time_us;
                    summary.payload_time_us = ev.result->payload_time_us;
                    summary.resample_time_us = ev.result->resample_time_us;
                    summary.retry_time_us = ev.result->retry_time_us;
                }
                if (std::getenv("STREAMING_HARNESS_DEBUG")) {
                    std::cerr << "[debug] FrameDone event success="
                              << ((ev.result && ev.result->success) ? "yes" : "no");
                    if (ev.result) {
                        std::cerr << " payload_crc_ok=" << (ev.result->payload_crc_ok ? "yes" : "no")
                                  << " payload_len=" << ev.result->payload.size();
                        std::cerr << " sr_ratio_used=" << ev.result->sample_rate_ratio_used;
                    }
                    std::cerr << '\n';
                }
                break;
            case lora::StreamingReceiver::FrameEvent::Type::FrameError:
                frame_done = true;
                frame_error = true;
                if (std::getenv("STREAMING_HARNESS_DEBUG")) {
                    std::cerr << "[debug] FrameError event message=" << ev.message << '\n';
                }
                break;
            default:
                break;
            }
        }
        if (args.throttle_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(args.throttle_us));
        }
    };

    // Optional idle gap before the frame, synthesized as zeros to exercise
    // buffer growth and symbol flush behavior between frames.
    if (gap_samples_before > 0) {
        for (std::size_t ofs = 0; ofs < gap_samples_before; ofs += chunk) {
            const std::size_t take = std::min(chunk, gap_samples_before - ofs);
            feed_chunk(std::span<const lora::IqLoader::Sample>(zero_buffer.data(), take));
        }
    }

    // Feed the actual frame until we either finish or run out of samples.
    for (std::size_t ofs = 0; ofs < samples.size(); ofs += chunk) {
        const std::size_t take = std::min(chunk, samples.size() - ofs);
        feed_chunk(std::span<const lora::IqLoader::Sample>(samples.data() + ofs, take));
        if (frame_done) {
            break;
        }
    }

    // If not finished yet, append a small idle tail (guard) to allow decoders
    // to flush any pending symbols; size based on sps and gap_symbols.
    if (!frame_done) {
        const std::size_t flush_sps = static_cast<std::size_t>(meta.fs / meta.bw);
        const std::size_t flush_samples = flush_sps * std::max<std::size_t>(2, args.gap_symbols);
        for (std::size_t ofs = 0; ofs < flush_samples; ofs += chunk) {
            const std::size_t take = std::min(chunk, flush_samples - ofs);
            feed_chunk(std::span<const lora::IqLoader::Sample>(zero_buffer.data(), take));
            if (frame_done) {
                break;
            }
        }
    }

    summary.bytes_emitted = bytes_emitted;
    if (!frame_done) {
        summary.success = false;
    }
    if (frame_error) {
        summary.success = false;
    }

    if (std::getenv("STREAMING_HARNESS_DEBUG")) {
        std::cerr << "[debug] frame_done=" << frame_done
                  << " frame_error=" << frame_error
                  << " success=" << (summary.success ? "yes" : "no")
                  << " payload_len=" << summary.payload_len
                  << " bytes=" << summary.bytes_emitted
                  << " sr_hint=" << summary.sample_rate_ratio_hint
                  << " sr_used=" << summary.sample_rate_ratio_used
                  << '\n';
    }

    return {summary.success, summary};
}

} // namespace

int main(int argc, char **argv) {
    try {
        Args args = parse_args(argc, argv);

        std::vector<FrameSummary> summaries;
        summaries.reserve(args.inputs.size());

        std::ofstream csv_file;
        bool csv_enabled = false;
        if (args.csv_path.has_value()) {
            const auto &csv_path = *args.csv_path;
            const bool exists = std::filesystem::exists(csv_path);
            csv_file.open(csv_path, std::ios::app);
            if (!csv_file) {
                throw std::runtime_error("failed to open csv file: " + csv_path.string());
            }
            bool needs_header = true;
            if (exists) {
                try {
                    needs_header = std::filesystem::file_size(csv_path) == 0;
                } catch (...) {
                    needs_header = false;
                }
            }
            if (needs_header) {
                csv_file << "vector,sf,bw_hz,fs_hz,cr,ldro,implicit,crc,chunk_size,throttle_us,chunks,avg_chunk_us,total_chunk_ms,sr_hint_ratio,sr_used_ratio,sr_scan_attempts,sr_scan_successes,cfo_sweep_attempts,cfo_sweep_successes,payload_retries,used_cached_sr,sync_time_us,header_time_us,payload_time_us,resample_time_us,retry_time_us,success,payload_len,payload_bytes\n";
            }
            csv_enabled = true;
        }

        std::optional<std::size_t> gap_samples = std::nullopt;
        bool all_ok = true;

        // Iterate inputs; compute gap in samples from gap_symbols once using
        // the first frame's parameters, then reuse between frames.
        for (std::size_t idx = 0; idx < args.inputs.size(); ++idx) {
            const auto &input = args.inputs[idx];
            auto meta = load_metadata(input, args);
            auto samples = load_vector(input);

            if (!gap_samples.has_value()) {
                const std::size_t sps = static_cast<std::size_t>(meta.fs / meta.bw);
                gap_samples = args.gap_symbols * sps;
            }

            const std::size_t gap_before = (idx == 0) ? 0 : *gap_samples;
            auto [ok, summary] = run_frame(meta, args, samples, gap_before);
            summaries.push_back(summary);
            all_ok = all_ok && ok;

            // Per-frame report for quick scanning in logs.
            const double avg_chunk_us = summary.chunk_count > 0
                                             ? summary.total_push_us / static_cast<double>(summary.chunk_count)
                                             : 0.0;
            std::cout << "[frame " << (idx + 1) << "] " << summary.name
                      << " sf=" << meta.sf << " bw=" << meta.bw
                      << " fs=" << meta.fs << " cr=" << meta.cr
                      << " implicit=" << (meta.implicit ? "yes" : "no")
                      << " crc=" << (meta.crc ? "yes" : "no")
                      << " -> success=" << (summary.success ? "yes" : "no")
                      << " payload_len=" << summary.payload_len
                      << " payload_bytes_events=" << summary.bytes_emitted
                      << " chunks=" << summary.chunk_count
                      << " avg_chunk_us=" << std::fixed << std::setprecision(2) << avg_chunk_us
                      << " sr_hint=" << std::setprecision(6) << summary.sample_rate_ratio_hint
                      << " sr_used=" << summary.sample_rate_ratio_used
                      << " sr_scan=" << summary.sr_scan_successes << "/" << summary.sr_scan_attempts
                      << " cfo_sweep=" << summary.cfo_sweep_successes << "/" << summary.cfo_sweep_attempts
                      << " payload_retries=" << summary.payload_retry_attempts
                      << " cached_sr=" << (summary.used_cached_sample_rate ? "yes" : "no")
                      << " t_sync_us=" << summary.sync_time_us
                      << " t_header_us=" << summary.header_time_us
                      << " t_payload_us=" << summary.payload_time_us
                      << " t_resample_us=" << summary.resample_time_us
                      << " t_retry_us=" << summary.retry_time_us
                      << '\n';
            std::cout.unsetf(std::ios::floatfield);
            std::cout << std::setprecision(6);

            if (csv_enabled) {
                const double frame_total_ms = summary.total_push_us / 1000.0;
                std::ostringstream row;
                row << meta.path.string() << ','
                    << meta.sf << ','
                    << meta.bw << ','
                    << meta.fs << ','
                    << meta.cr << ','
                    << ldro_mode_to_int(meta.ldro_mode) << ','
                    << (meta.implicit ? 1 : 0) << ','
                    << (meta.crc ? 1 : 0) << ','
                    << args.chunk << ','
                    << args.throttle_us << ','
                    << summary.chunk_count << ','
                    << std::fixed << std::setprecision(2) << avg_chunk_us << ','
                    << frame_total_ms << ','
                    << std::setprecision(9) << summary.sample_rate_ratio_hint << ','
                    << summary.sample_rate_ratio_used << ','
                    << summary.sr_scan_attempts << ','
                    << summary.sr_scan_successes << ','
                    << summary.cfo_sweep_attempts << ','
                    << summary.cfo_sweep_successes << ','
                    << summary.payload_retry_attempts << ','
                    << (summary.used_cached_sample_rate ? 1 : 0) << ','
                    << std::fixed << std::setprecision(2) << summary.sync_time_us << ','
                    << summary.header_time_us << ','
                    << summary.payload_time_us << ','
                    << summary.resample_time_us << ','
                    << summary.retry_time_us << ','
                    << (summary.success ? 1 : 0) << ','
                    << summary.payload_len << ','
                    << summary.bytes_emitted
                    << '\n';
                csv_file << row.str();
                csv_file.flush();
            }
        }

        // Aggregate summary across all frames; return nonzero exit on failure.
        std::size_t ok_count = 0;
        std::size_t fail_count = 0;
        std::size_t total_bytes = 0;
        std::size_t total_chunks = 0;
        double total_push_us = 0.0;
        for (const auto &s : summaries) {
            total_bytes += s.bytes_emitted;
            total_chunks += s.chunk_count;
            total_push_us += s.total_push_us;
            if (s.success) {
                ++ok_count;
            } else {
                ++fail_count;
            }
        }

        const double avg_chunk_us = total_chunks > 0 ? total_push_us / static_cast<double>(total_chunks) : 0.0;
        const double total_ms = total_push_us / 1000.0;

        std::cout << "[summary] frames_ok=" << ok_count
                  << " frames_failed=" << fail_count
                  << " payload_bytes=" << total_bytes
                  << " total_chunk_ms=" << std::fixed << std::setprecision(2) << total_ms
                  << " avg_chunk_us=" << avg_chunk_us << '\n';
        std::cout.unsetf(std::ios::floatfield);
        return all_ok ? 0 : 1;
    } catch (const std::exception &ex) {
        std::cerr << "[ERROR] " << ex.what() << '\n';
        return 2;
    }
}
