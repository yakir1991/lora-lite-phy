#include "receiver.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct ParsedArgs {
    std::filesystem::path input;
    int sf = 7;
    int bandwidth_hz = 125000;
    int sample_rate_hz = 500000;
    bool ldro_enabled = false;
    bool debug = false;
    bool implicit_header = false;
    int payload_length = 0;
    int coding_rate = 1;
    bool has_crc = true;
    unsigned sync_word = 0x12u;
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
              << "  --debug                 Print extra diagnostics\n";
}

ParsedArgs parse_args(int argc, char **argv) {
    ParsedArgs args;
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
        auto parsed = parse_args(argc, argv);
        lora::DecodeParams params;
        params.sf = parsed.sf;
        params.bandwidth_hz = parsed.bandwidth_hz;
        params.sample_rate_hz = parsed.sample_rate_hz;
        params.ldro_enabled = parsed.ldro_enabled;
        params.sync_word = parsed.sync_word;
        params.implicit_header = parsed.implicit_header;
        params.implicit_payload_length = parsed.payload_length;
        params.implicit_has_crc = parsed.has_crc;
        params.implicit_cr = parsed.coding_rate;

        if (parsed.implicit_header) {
            if (params.implicit_payload_length <= 0) {
                throw std::runtime_error("Implicit header requires --payload-len > 0");
            }
            if (params.implicit_cr < 1 || params.implicit_cr > 4) {
                throw std::runtime_error("Implicit header requires --cr in range 1-4");
            }
        }

        lora::Receiver receiver(params);
        const auto samples = lora::IqLoader::load_cf32(parsed.input);
        const auto result = receiver.decode_samples(samples);

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
        return result.success ? 0 : 1;
    } catch (const std::exception &ex) {
        std::cerr << "[ERROR] " << ex.what() << '\n';
        print_usage(argv[0]);
        return 2;
    }
}
