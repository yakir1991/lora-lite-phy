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
};

void print_usage(const char *prog) {
    std::cout << "Usage: " << prog << " [options] <file.cf32>\n"
              << "Options:\n"
              << "  --sf <int>              Spreading factor (default 7)\n"
             << "  --bw <int>              Bandwidth in Hz (default 125000)\n"
             << "  --fs <int>              Sample rate in Hz (default 500000)\n"
             << "  --ldro <0|1>            Enable LDRO (default 0)\n"
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
