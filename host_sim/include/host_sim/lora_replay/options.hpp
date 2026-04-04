#pragma once

#include <filesystem>
#include <optional>
#include <string>

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
    std::optional<std::filesystem::path> dump_payload;
    std::optional<std::filesystem::path> summary_output;
    bool multi_packet{false};
    bool soft{false};
    bool verbose{false};
    bool stream{false};
    bool per_stats{false};
    bool multi_sf{false};
    float cfo_track_alpha{0.0f};
    enum class IqFormat { cf32, hackrf } iq_format{IqFormat::cf32};
    bool read_stdin{false};
};

void print_usage(const char* binary);
Options parse_arguments(int argc, char** argv);

} // namespace host_sim::lora_replay
