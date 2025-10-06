#include "receiver.hpp"
#include "streaming_receiver.hpp"
#include "frame_sync.hpp"
#include "sync_word_detector.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <span>
#include <cstddef>
#include <iomanip>

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
    bool ldro_enabled = false;
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
};

void print_usage(const char *prog) {
    std::cout << "Usage: " << prog << " [options] <file.cf32>\n"
              << "Options:\n"
              << "  --sf <int>              Spreading factor (default 7)\n"
              << "  --bw <int>              Bandwidth in Hz (default 125000)\n"
              << "  --fs <int>              Sample rate in Hz (default 500000)\n"
              << "  --ldro <0|1>            Enable LDRO (default 0)\n"
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
        } else if (cur == "--ldro" && i + 1 < argc) {
            args.ldro_enabled = std::stoi(argv[++i]) != 0;
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

} // namespace

int main(int argc, char **argv) {
    try {
        // 1) Parse CLI and map to library DecodeParams.
        auto parsed = parse_args(argc, argv);
        lora::DecodeParams params;
        params.sf = parsed.sf;
        params.bandwidth_hz = parsed.bandwidth_hz;
        params.sample_rate_hz = parsed.sample_rate_hz;
        params.ldro_enabled = parsed.ldro_enabled;
        params.sync_word = parsed.sync_word;
        params.skip_sync_word_check = parsed.skip_sync_word_check;
        params.implicit_header = parsed.implicit_header;
        params.implicit_payload_length = parsed.payload_length;
        params.implicit_has_crc = parsed.has_crc;
        params.implicit_cr = parsed.coding_rate;
        params.emit_payload_bytes = parsed.emit_payload_bytes;

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

            while (offset < samples.size()) {
                const std::size_t take = std::min(chunk, samples.size() - offset);
                std::span<const lora::StreamingReceiver::Sample> span(&samples[offset], take);
                auto events = streaming.push_samples(span);
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

            return frame_done ? 0 : 1;
        }

        lora::Receiver receiver(params);
        const auto samples = lora::IqLoader::load_cf32(parsed.input);
        const auto result = receiver.decode_samples(samples);

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
        // Return 0 only if end-to-end decode succeeded; 1 otherwise.
        return result.success ? 0 : 1;
    } catch (const std::exception &ex) {
        // CLI/argument/I-O errors: report and return 2. Usage is printed for convenience.
        std::cerr << "[ERROR] " << ex.what() << '\n';
        print_usage(argv[0]);
        return 2;
    }
}
