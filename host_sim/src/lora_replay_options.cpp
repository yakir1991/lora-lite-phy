#include "host_sim/lora_replay/options.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace host_sim::lora_replay
{

void print_usage(const char* binary)
{
    std::cerr << "Usage: " << binary << " --iq <capture.cf32 | ->"
              << " [--format cf32|hackrf]"
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
              << " [--verbose]"
              << "\n"
              << "\n  --iq -           Read IQ samples from stdin (pipe mode)"
              << "\n  --format hackrf  Expect HackRF int8 IQ on stdin (default: cf32)"
              << "\n";
}

Options parse_arguments(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--iq" && i + 1 < argc) {
            opts.iq_file = argv[++i];
            if (opts.iq_file == "-") {
                opts.read_stdin = true;
            }
        } else if (arg == "--format" && i + 1 < argc) {
            const std::string_view fmt{argv[++i]};
            if (fmt == "hackrf" || fmt == "int8") {
                opts.iq_format = Options::IqFormat::hackrf;
            } else if (fmt == "cf32") {
                opts.iq_format = Options::IqFormat::cf32;
            } else {
                throw std::runtime_error("Unknown IQ format: " + std::string(fmt) +
                                         " (expected cf32 or hackrf)");
            }
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
        } else if (arg == "--verbose" || arg == "-v") {
            opts.verbose = true;
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

} // namespace host_sim::lora_replay
