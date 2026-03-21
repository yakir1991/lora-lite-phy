#pragma once

#include "host_sim/capture.hpp"
#include "host_sim/impairments.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace host_sim::lora_replay
{

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
    std::optional<std::filesystem::path> dump_normalized;
    std::optional<std::filesystem::path> dump_bins;
    std::optional<std::filesystem::path> summary_output;
    std::optional<std::filesystem::path> dump_payload;
    std::optional<std::filesystem::path> dump_crc;
    std::optional<std::filesystem::path> sync_offset_file;
    int payload_start_adjust{0};
    bool bypass_crc_verif{false};
    bool allow_stage_mismatch{false};
    struct McuTarget
    {
        std::string name;
        double freq_mhz{0.0};
    };
    double ns_to_cycle_scale{0.0};
    std::vector<McuTarget> mcu_targets;
    std::string instrumentation_numeric_mode{"float"};
    bool enable_dma_sim{false};
    double dma_fill_ns{0.0};
    double dma_jitter_ns{0.0};
    double isr_latency_ns{0.0};
    int isr_every_symbols{0};
    bool real_time_mode{false};
    double rt_speed{1.0};
    double rt_tolerance_ns{0.0};
    std::size_t rt_max_events{64};
    host_sim::ImpairmentConfig impairment;
};

void print_usage(const char* binary);

Options parse_arguments(int argc, char** argv);

void write_stats_json(const std::filesystem::path& path,
                      const host_sim::CaptureStats& stats,
                      const Options& opts);

} // namespace host_sim::lora_replay
