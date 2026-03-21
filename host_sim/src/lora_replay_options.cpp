#include "host_sim/lora_replay/options.hpp"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace host_sim::lora_replay
{

void print_usage(const char* binary)
{
    std::cerr << "Usage: " << binary << " --iq <capture.cf32>"
              << " [--metadata <file.json>]"
              << " [--payload <ascii>]"
              << " [--stats <file.json>]"
              << " [--dump-symbols <file.txt>]"
              << " [--dump-iq <file.cf32>]"
              << " [--dump-payload <file.bin>]"
              << " [--compare-root <path/prefix>]"
              << " [--dump-stages <path/prefix>]"
              << " [--summary <file.json>]"
              << " [--bypass-crc-verif]"
              << " [--payload-start-adjust <symbols>]"
              << " [--ns-to-cycle <cycles_per_ns>]"
              << " [--instrument-mode <float|q15>]"
              << " [--mcu-target <name:freq_mhz>]"
              << " [--rt]"
              << " [--rt-speed <factor>]"
              << " [--rt-tolerance-ns <ns>]"
              << " [--rt-max-events <count>]"
              << " [--impair-cfo-ppm <ppm>]"
              << " [--impair-cfo-drift-ppm <ppm_per_s>]"
              << " [--impair-sfo-ppm <ppm>]"
              << " [--impair-sfo-drift-ppm <ppm_per_s>]"
              << " [--impair-awgn-snr <dB>]"
              << " [--impair-burst-period <symbols>]"
              << " [--impair-burst-duration <symbols>]"
              << " [--impair-burst-snr <dB>]"
              << " [--impair-seed <value>]"
              << " [--impair-collision-prob <probability>]"
              << " [--impair-collision-scale <scale>]"
              << " [--impair-collision-file <cf32_path>]"
              << '\n';
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
        } else if (arg == "--dump-normalized" && i + 1 < argc) {
            opts.dump_normalized = std::filesystem::path{argv[++i]};
        } else if (arg == "--dump-bins" && i + 1 < argc) {
            opts.dump_bins = std::filesystem::path{argv[++i]};
        } else if (arg == "--summary" && i + 1 < argc) {
            opts.summary_output = std::filesystem::path{argv[++i]};
        } else if (arg == "--dump-payload" && i + 1 < argc) {
            opts.dump_payload = std::filesystem::path{argv[++i]};
        } else if (arg == "--dump-crc" && i + 1 < argc) {
            opts.dump_crc = std::filesystem::path{argv[++i]};
        } else if (arg == "--sync-offsets" && i + 1 < argc) {
            opts.sync_offset_file = std::filesystem::path{argv[++i]};
        } else if (arg == "--payload-start-adjust" && i + 1 < argc) {
            opts.payload_start_adjust = std::stoi(argv[++i]);
        } else if (arg == "--bypass-crc-verif") {
            opts.bypass_crc_verif = true;
        } else if (arg == "--ns-to-cycle" && i + 1 < argc) {
            opts.ns_to_cycle_scale = std::stod(argv[++i]);
        } else if (arg == "--instrument-mode" && i + 1 < argc) {
            opts.instrumentation_numeric_mode = argv[++i];
        } else if (arg == "--dma-fill-ns" && i + 1 < argc) {
            opts.enable_dma_sim = true;
            opts.dma_fill_ns = std::stod(argv[++i]);
        } else if (arg == "--dma-jitter-ns" && i + 1 < argc) {
            opts.enable_dma_sim = true;
            opts.dma_jitter_ns = std::stod(argv[++i]);
        } else if (arg == "--isr-latency-ns" && i + 1 < argc) {
            opts.isr_latency_ns = std::stod(argv[++i]);
        } else if (arg == "--isr-every-symbols" && i + 1 < argc) {
            opts.isr_every_symbols = std::stoi(argv[++i]);
        } else if (arg == "--rt") {
            opts.real_time_mode = true;
        } else if (arg == "--rt-speed" && i + 1 < argc) {
            opts.rt_speed = std::stod(argv[++i]);
        } else if (arg == "--rt-tolerance-ns" && i + 1 < argc) {
            opts.rt_tolerance_ns = std::stod(argv[++i]);
        } else if (arg == "--rt-max-events" && i + 1 < argc) {
            opts.rt_max_events = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--impair-cfo-ppm" && i + 1 < argc) {
            opts.impairment.cfo_ppm = std::stod(argv[++i]);
        } else if (arg == "--mcu-target" && i + 1 < argc) {
            std::string spec{argv[++i]};
            auto sep = spec.find(':');
            if (sep == std::string::npos) {
                throw std::runtime_error("--mcu-target requires format name:freq_mhz");
            }
            std::string name = spec.substr(0, sep);
            if (name.empty()) {
                name = "mcu";
            }
            const std::string freq_str = spec.substr(sep + 1);
            if (freq_str.empty()) {
                throw std::runtime_error("--mcu-target missing frequency component");
            }
            opts.mcu_targets.push_back({name, std::stod(freq_str)});
        } else if (arg == "--impair-cfo-drift-ppm" && i + 1 < argc) {
            opts.impairment.cfo_drift_ppm_per_s = std::stod(argv[++i]);
        } else if (arg == "--impair-sfo-ppm" && i + 1 < argc) {
            opts.impairment.sfo_ppm = std::stod(argv[++i]);
        } else if (arg == "--impair-sfo-drift-ppm" && i + 1 < argc) {
            opts.impairment.sfo_drift_ppm_per_s = std::stod(argv[++i]);
        } else if (arg == "--impair-awgn-snr" && i + 1 < argc) {
            opts.impairment.awgn_enabled = true;
            opts.impairment.awgn_snr_db = std::stod(argv[++i]);
        } else if (arg == "--impair-burst-period" && i + 1 < argc) {
            opts.impairment.burst.enabled = true;
            opts.impairment.burst.period_symbols = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--impair-burst-duration" && i + 1 < argc) {
            opts.impairment.burst.enabled = true;
            opts.impairment.burst.duration_symbols = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--impair-burst-snr" && i + 1 < argc) {
            opts.impairment.burst.enabled = true;
            opts.impairment.burst.snr_db = std::stod(argv[++i]);
        } else if (arg == "--impair-seed" && i + 1 < argc) {
            opts.impairment.seed = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--impair-collision-prob" && i + 1 < argc) {
            opts.impairment.collision.enabled = true;
            opts.impairment.collision.probability = std::stod(argv[++i]);
        } else if (arg == "--impair-collision-scale" && i + 1 < argc) {
            opts.impairment.collision.enabled = true;
            opts.impairment.collision.scale = std::stod(argv[++i]);
        } else if (arg == "--impair-collision-file" && i + 1 < argc) {
            opts.impairment.collision.enabled = true;
            opts.impairment.collision.waveform_path = argv[++i];
        } else if (arg == "--allow-stage-mismatch") {
            opts.allow_stage_mismatch = true;
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

} // namespace host_sim::lora_replay
