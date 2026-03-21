#pragma once

#include "host_sim/capture.hpp"
#include "host_sim/lora_params.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <string>
#include <stdexcept>
#include <type_traits>
#include <tuple>
#include <vector>
#include <limits>

namespace host_sim::lora_replay
{

struct HeaderDecodeResult
{
    bool success{false};
    int payload_len{0};
    int cr{0};
    bool has_crc{false};
    std::optional<bool> ldro_enabled;
    int consumed_symbols{0};
    int checksum_field{0};
    int checksum_computed{0};
    std::vector<uint16_t> codewords;
    std::vector<uint8_t> nibbles;
};

struct StageOutputs
{
    std::vector<uint16_t> fft;
    std::vector<uint16_t> gray;
    std::vector<uint16_t> deinterleaver;
    std::vector<uint8_t> hamming;
};

uint16_t normalize_fft_symbol(uint16_t symbol, bool reduce_by_four, int sf);

void append_fft_gray(const std::vector<uint16_t>& block_symbols,
                     bool is_header_block,
                     bool ldro_enabled,
                     int sf,
                     StageOutputs& outputs);

std::vector<long long> read_stage_file(const std::filesystem::path& path);

template <typename Value>
void write_stage_file(const std::filesystem::path& path, const std::vector<Value>& values)
{
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Failed to write stage dump: " + path.string());
    }
    for (auto value : values) {
        out << static_cast<long long>(value) << '\n';
    }
}

struct StageComparisonResult
{
    std::string label;
    std::size_t host_count{0};
    std::size_t ref_count{0};
    std::size_t mismatches{0};
    std::optional<std::size_t> alignment_offset;
    bool alignment_relative_to_reference{true};
    std::optional<std::size_t> first_diff_index;
    std::optional<long long> host_value;
    std::optional<long long> ref_value;
    bool reference_missing{false};
};

template <typename HostType>
StageComparisonResult compare_stage(const std::string& label,
                                    const std::vector<HostType>& host,
                                    const std::vector<long long>& reference)
{
    StageComparisonResult result;
    result.label = label;
    result.host_count = host.size();
    result.ref_count = reference.size();

    if (host.empty()) {
        if (!reference.empty()) {
            result.mismatches = reference.size();
            result.alignment_offset = 0;
            result.first_diff_index = 0;
            result.ref_value = reference.front();
        }
        return result;
    }

    if (reference.empty()) {
        result.mismatches = result.host_count;
        result.alignment_relative_to_reference = false;
        result.alignment_offset = 0;
        result.first_diff_index = 0;
        result.host_value = static_cast<long long>(host.front());
        return result;
    }

    const bool host_is_longer = result.host_count >= result.ref_count;
    const std::size_t compare_len = host_is_longer ? result.ref_count : result.host_count;
    if (compare_len == 0) {
        return result;
    }

    const std::size_t max_offset =
        host_is_longer ? (result.host_count - compare_len) : (result.ref_count - compare_len);

    auto evaluate_alignment = [&](std::size_t host_start, std::size_t ref_start) {
        std::size_t mismatches = 0;
        std::optional<std::size_t> first_diff;
        std::optional<long long> first_host;
        std::optional<long long> first_ref;
        for (std::size_t idx = 0; idx < compare_len; ++idx) {
            const long long host_val = static_cast<long long>(host[host_start + idx]);
            const long long ref_val = reference[ref_start + idx];
            if (host_val != ref_val) {
                ++mismatches;
                if (!first_diff) {
                    first_diff = idx;
                    first_host = host_val;
                    first_ref = ref_val;
                }
            }
        }
        return std::tuple<std::size_t,
                          std::optional<std::size_t>,
                          std::optional<long long>,
                          std::optional<long long>>(mismatches, first_diff, first_host, first_ref);
    };

    std::size_t best_offset = 0;
    std::size_t best_mismatches = std::numeric_limits<std::size_t>::max();
    std::optional<std::size_t> best_first_diff;
    std::optional<long long> best_host_value;
    std::optional<long long> best_ref_value;

    for (std::size_t offset = 0; offset <= max_offset; ++offset) {
        const std::size_t host_start = host_is_longer ? offset : 0;
        const std::size_t ref_start = host_is_longer ? 0 : offset;
        auto [mism, first_diff, host_val, ref_val] = evaluate_alignment(host_start, ref_start);
        if (mism < best_mismatches) {
            best_mismatches = mism;
            best_offset = offset;
            best_first_diff = first_diff;
            best_host_value = host_val;
            best_ref_value = ref_val;
            if (best_mismatches == 0) {
                break;
            }
        }
    }

    result.mismatches = (best_mismatches == std::numeric_limits<std::size_t>::max())
                            ? compare_len
                            : best_mismatches;
    result.alignment_offset = best_offset;
    result.alignment_relative_to_reference = !host_is_longer;
    if (best_first_diff) {
        result.first_diff_index = best_first_diff;
        result.host_value = best_host_value;
        result.ref_value = best_ref_value;
    }

    return result;
}

struct SummaryReport
{
    std::optional<std::string> capture_path;
    std::optional<host_sim::LoRaMetadata> metadata;
    std::optional<host_sim::CaptureStats> stats;
    std::vector<uint16_t> preview_symbols;
    std::vector<StageComparisonResult> stage_results;
    std::size_t reference_mismatches{0};
    std::size_t stage_mismatches{0};
    bool whitening_roundtrip_ok{true};
    bool compare_run{false};
    std::vector<double> stage_timings_ns;
    std::vector<double> stage_cycles;
    std::vector<std::size_t> memory_usage_bytes;
    std::vector<std::size_t> stage_scratch_bytes;
    std::vector<std::string> stage_labels;
    std::size_t max_symbol_memory_bytes{0};
    double cycle_scale_ns_to_cycles{0.0};
    std::size_t stage_count{0};
    std::size_t instrumented_symbols{0};
    std::string instrumentation_numeric_mode{"float"};
    double avg_symbol_duration_ns{0.0};
    double max_symbol_duration_ns{0.0};
    double symbol_duration_target_ns{0.0};
    double min_deadline_margin_ns{0.0};
    std::size_t deadline_miss_count{0};
    double acquisition_time_us{0.0};
    double tracking_jitter_us{0.0};
    double packet_error_rate{0.0};
    double bit_error_rate{0.0};
    bool tracking_failure{false};
    std::string tracking_failure_reason;
    std::string tracking_mitigation;
    struct McuCycleSummary
    {
        std::string name;
        double freq_mhz{0.0};
        double cycles_per_symbol_budget{0.0};
        double cycles_per_symbol_avg{0.0};
        double cycles_per_symbol_max{0.0};
        double utilisation_avg{0.0};
        double utilisation_max{0.0};
    };
    std::vector<McuCycleSummary> mcu_cycle_summaries;
    bool realtime_mode{false};
    double rt_symbol_period_ns{0.0};
    double rt_avg_start_lag_ns{0.0};
    double rt_max_start_lag_ns{0.0};
    double rt_avg_deadline_margin_ns{0.0};
    double rt_min_deadline_margin_ns{0.0};
    std::size_t rt_overrun_count{0};
    std::size_t rt_underrun_count{0};
    struct RealTimeEvent
    {
        std::size_t symbol_index{0};
        double timestamp_ns{0.0};
        double delta_ns{0.0};
    };
    std::vector<RealTimeEvent> rt_overrun_events;
    std::vector<RealTimeEvent> rt_underrun_events;
    struct StageInstrumentationEntry
    {
        std::string label;
        std::size_t index{0};
        double avg_ns{0.0};
        double max_ns{0.0};
        double avg_cycles{0.0};
        double max_cycles{0.0};
        std::size_t max_scratch_bytes{0};
    };
    std::vector<StageInstrumentationEntry> stage_instrumentation;
};

std::string build_stage_summary_token(const StageComparisonResult& result);

struct StageDiffReport
{
    std::string summary_line;
    std::string ci_line;
    std::size_t total_mismatches{0};
    bool any_missing{false};
};

StageDiffReport build_stage_diff_report(const std::vector<StageComparisonResult>& results,
                                        bool verbose,
                                        std::ostream& stream);

void write_summary_json(const std::filesystem::path& path, const SummaryReport& report);

std::vector<StageComparisonResult> compare_with_reference(const StageOutputs& outputs,
                                                          const std::filesystem::path& compare_root);

uint16_t compute_lora_crc(const std::vector<uint8_t>& payload);

// Computes the GNU Radio (gr-lora_sdr) CRC value for a payload window:
// CRC16-CCITT (poly 0x1021, init 0x0000) over the first (N-2) bytes, XORed
// with the last two payload bytes interpreted as MSB:LSB.
// This is the value gr-lora_sdr compares against the received CRC bytes.
uint16_t compute_lora_crc_syndrome(const std::vector<uint8_t>& payload_with_crc);

} // namespace host_sim::lora_replay
