#include "iq_loader.hpp"
#include "streaming_receiver.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fstream>
#include <cctype>

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

// CLI options with reasonable defaults for common LoRa settings.
struct Args {
    int fallback_sf = 7;
    int fallback_bw = 125000;
    int fallback_fs = 500000;
    int fallback_cr = 1;
    bool fallback_ldro = false;
    unsigned fallback_sync = 0x12u;
    bool emit_bytes = false;
    std::size_t chunk = 2048;
    std::size_t gap_symbols = 8;
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
              << "  --ldro               Fallback LDRO flag\n"
              << "  --sync-word <hex>    Fallback sync word (default 0x12)\n"
              << "  --emit-bytes         Emit payload byte events\n"
              << "  --chunk <int>        Chunk size (default 2048 samples)\n"
              << "  --gap-symbols <int>  Idle symbols between frames (default 8)\n";
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
            args.fallback_ldro = true;
        } else if (cur == "--sync-word" && i + 1 < argc) {
            args.fallback_sync = static_cast<unsigned>(std::stoul(argv[++i], nullptr, 0));
        } else if (cur == "--emit-bytes") {
            args.emit_bytes = true;
        } else if (cur == "--chunk" && i + 1 < argc) {
            args.chunk = std::max<std::size_t>(1, std::stoul(argv[++i]));
        } else if (cur == "--gap-symbols" && i + 1 < argc) {
            args.gap_symbols = std::stoul(argv[++i]);
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
    bool ldro;
    bool implicit;
    bool crc;
    unsigned sync;
    std::string payload_hex;
};

// Attempt to read a .json sidecar next to the .cf32; fallback to CLI defaults
// for any missing fields. This allows heterogeneous vectors in a single run.
FrameMeta load_metadata(const std::filesystem::path &path, const Args &defaults) {
    FrameMeta meta{path,
                   defaults.fallback_sf,
                   defaults.fallback_bw,
                   defaults.fallback_fs,
                   defaults.fallback_cr,
                   defaults.fallback_ldro,
                   false,
                   true,
                   defaults.fallback_sync,
                   {}};
    auto json_path = path;
    json_path.replace_extension(".json");
    if (std::filesystem::exists(json_path)) {
        SimpleJson json(json_path);
        if (auto sf_opt = json.get<int>("sf")) meta.sf = *sf_opt;
        if (auto bw_opt = json.get<int>("bw")) meta.bw = *bw_opt;
        if (auto fs_opt = json.get<int>("sample_rate")) meta.fs = *fs_opt;
        if (auto fs_opt = json.get<int>("samp_rate")) meta.fs = *fs_opt;
        if (auto cr_opt = json.get<int>("cr")) meta.cr = *cr_opt;
        if (auto ldro_opt = json.get<bool>("ldro_mode")) meta.ldro = *ldro_opt;
        if (auto impl_opt = json.get<bool>("impl_header")) meta.implicit = *impl_opt;
        if (auto impl_opt = json.get<bool>("implicit_header")) meta.implicit = *impl_opt;
        if (auto crc_opt = json.get<bool>("crc")) meta.crc = *crc_opt;
        if (auto sync_opt = json.get<unsigned>("sync_word")) meta.sync = *sync_opt;
        if (auto payload_hex_opt = json.get<std::string>("payload_hex")) meta.payload_hex = *payload_hex_opt;
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
    params.ldro_enabled = meta.ldro;
    params.sync_word = meta.sync;
    params.skip_sync_word_check = false;
    params.implicit_header = meta.implicit;
    params.implicit_payload_length = 0;
    params.implicit_has_crc = meta.crc;
    params.implicit_cr = meta.cr;
    params.emit_payload_bytes = args.emit_bytes;

    lora::StreamingReceiver receiver(params);

    const std::size_t chunk = args.chunk;
    FrameSummary summary{};
    summary.name = meta.path.filename().string();

    std::size_t bytes_emitted = 0;
    bool frame_done = false;
    bool frame_error = false;

    // Helper: push a chunk into the receiver and harvest events.
    auto feed_chunk = [&](std::span<const lora::IqLoader::Sample> span) {
        auto events = receiver.push_samples(span);
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
                break;
            case lora::StreamingReceiver::FrameEvent::Type::FrameError:
                frame_done = true;
                frame_error = true;
                break;
            default:
                break;
            }
        }
    };

    // Optional idle gap before the frame, synthesized as zeros to exercise
    // buffer growth and symbol flush behavior between frames.
    if (gap_samples_before > 0) {
        std::vector<lora::IqLoader::Sample> zeros(gap_samples_before, lora::IqLoader::Sample{0.0f, 0.0f});
        for (std::size_t ofs = 0; ofs < zeros.size(); ofs += chunk) {
            const std::size_t take = std::min(chunk, zeros.size() - ofs);
            feed_chunk(std::span<const lora::IqLoader::Sample>(zeros.data() + ofs, take));
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
        std::vector<lora::IqLoader::Sample> zeros(flush_samples, lora::IqLoader::Sample{0.0f, 0.0f});
        for (std::size_t ofs = 0; ofs < zeros.size(); ofs += chunk) {
            const std::size_t take = std::min(chunk, zeros.size() - ofs);
            feed_chunk(std::span<const lora::IqLoader::Sample>(zeros.data() + ofs, take));
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

    return {summary.success, summary};
}

} // namespace

int main(int argc, char **argv) {
    try {
        Args args = parse_args(argc, argv);

        std::vector<FrameSummary> summaries;
        summaries.reserve(args.inputs.size());

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
            std::cout << "[frame " << (idx + 1) << "] " << summary.name
                      << " sf=" << meta.sf << " bw=" << meta.bw
                      << " fs=" << meta.fs << " cr=" << meta.cr
                      << " implicit=" << (meta.implicit ? "yes" : "no")
                      << " crc=" << (meta.crc ? "yes" : "no")
                      << " -> success=" << (summary.success ? "yes" : "no")
                      << " payload_len=" << summary.payload_len
                      << " payload_bytes_events=" << summary.bytes_emitted
                      << '\n';
        }

        // Aggregate summary across all frames; return nonzero exit on failure.
        std::size_t ok_count = 0;
        std::size_t fail_count = 0;
        std::size_t total_bytes = 0;
        for (const auto &s : summaries) {
            total_bytes += s.bytes_emitted;
            if (s.success) {
                ++ok_count;
            } else {
                ++fail_count;
            }
        }

        std::cout << "[summary] frames_ok=" << ok_count
                  << " frames_failed=" << fail_count
                  << " payload_bytes=" << total_bytes << '\n';
        return all_ok ? 0 : 1;
    } catch (const std::exception &ex) {
        std::cerr << "[ERROR] " << ex.what() << '\n';
        return 2;
    }
}
