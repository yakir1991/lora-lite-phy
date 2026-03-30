#include "host_sim/lora_replay/stage_processing.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <ios>
#include <optional>
#include <sstream>

namespace host_sim::lora_replay
{
namespace
{

inline const char* json_bool(bool value)
{
    return value ? "true" : "false";
}

std::string json_escape(const std::string& value)
{
    std::string result;
    result.reserve(value.size() + 4);
    const char* hex = "0123456789abcdef";
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (ch < 0x20) {
                result += "\\u00";
                result += hex[(ch >> 4) & 0xF];
                result += hex[ch & 0xF];
            } else {
                result += static_cast<char>(ch);
            }
            break;
        }
    }
    return result;
}

} // namespace

uint16_t compute_lora_crc(const std::vector<uint8_t>& payload)
{
    // Match gr-lora_sdr's crc_verif block:
    // 1) Compute CRC16-CCITT over the first (N-2) bytes of the payload.
    // 2) XOR the result with the last two payload bytes, interpreted as MSB then LSB.
    // 3) Compare against the received CRC bytes that follow the payload.
    if (payload.size() < 2) {
        return 0x0000;
    }

    const std::size_t crc_input_len = payload.size() - 2;
    uint16_t crc = 0x0000;
    for (std::size_t idx = 0; idx < crc_input_len; ++idx) {
        const uint8_t byte = payload[idx];
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc <<= 1;
            }
        }
    }

    // XOR with the last two payload bytes (MSB then LSB).
    crc = static_cast<uint16_t>(crc ^ payload[payload.size() - 1] ^ (static_cast<uint16_t>(payload[payload.size() - 2]) << 8));
    return crc;
}

uint16_t compute_lora_crc_syndrome(const std::vector<uint8_t>& payload_with_crc)
{
    if (payload_with_crc.size() < 2) {
        return 0x0000;
    }
    const std::size_t data_len = payload_with_crc.size() - 2;

    uint16_t crc = 0x0000;
    for (std::size_t idx = 0; idx < data_len; ++idx) {
        const uint8_t byte = payload_with_crc[idx];
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc <<= 1;
            }
        }
    }

    // gr-lora_sdr's legacy "embedded CRC" syndrome:
    // CRC is embedded in the last two bytes (MSB then LSB) of the message window.
    const uint8_t embedded_lsb = payload_with_crc[data_len + 1];
    const uint8_t embedded_msb = payload_with_crc[data_len];
    const uint16_t embedded = static_cast<uint16_t>(embedded_msb) << 8 | static_cast<uint16_t>(embedded_lsb);
    return static_cast<uint16_t>(crc ^ embedded);
}

uint16_t normalize_fft_symbol(uint16_t symbol, bool reduce_by_four, int sf)
{
    const uint16_t mask_full = static_cast<uint16_t>((1u << sf) - 1u);
    uint16_t adjusted = static_cast<uint16_t>((symbol + mask_full) & mask_full);
    if (reduce_by_four) {
        adjusted = static_cast<uint16_t>(adjusted >> 2);
    }
    return adjusted;
}

void append_fft_gray(const std::vector<uint16_t>& block_symbols,
                     bool is_header_block,
                     bool ldro_enabled,
                     int sf,
                     StageOutputs& outputs)
{
    const bool reduce = ldro_enabled || is_header_block;
    for (uint16_t symbol : block_symbols) {
        const uint16_t fft_val = normalize_fft_symbol(symbol, reduce, sf);
        outputs.fft.push_back(fft_val);
        outputs.gray.push_back(static_cast<uint16_t>(fft_val ^ (fft_val >> 1u)));
    }
}

std::vector<long long> read_stage_file(const std::filesystem::path& path)
{
    std::vector<long long> values;
    std::ifstream input(path);
    if (!input) {
        return values;
    }
    long long value = 0;
    while (input >> value) {
        values.push_back(value);
    }
    return values;
}

std::string build_stage_summary_token(const StageComparisonResult& result)
{
    std::ostringstream token;
    if (result.reference_missing) {
        token << "missing";
        return token.str();
    }

    if (result.mismatches == 0) {
        token << "OK";
        return token.str();
    }

    token << "FAIL(" << result.mismatches;
    if (result.host_count > 0) {
        token << '/' << result.host_count;
    }
    if (result.first_diff_index) {
        token << "@idx" << *result.first_diff_index;
    }
    token << ')';
    return token.str();
}

StageDiffReport build_stage_diff_report(const std::vector<StageComparisonResult>& results,
                                        bool verbose,
                                        std::ostream& stream)
{
    StageDiffReport report;
    std::ostringstream summary;
    std::ostringstream ci;
    summary << "[compare] summary:";
    ci << "[ci-summary] stages";

    for (const auto& res : results) {
        if (verbose) {
            if (res.reference_missing) {
                stream << "[compare] stage " << res.label << ": reference file missing\n";
            } else {
                stream << "[compare] stage " << res.label << ": host=" << res.host_count
                       << " ref=" << res.ref_count << " mismatches=" << res.mismatches;
                if (res.alignment_offset) {
                    if (res.alignment_relative_to_reference) {
                        stream << " (aligned at ref offset " << *res.alignment_offset << ")";
                    } else {
                        stream << " (aligned at host offset " << *res.alignment_offset << ")";
                    }
                } else {
                    stream << " (no exact alignment, comparing from start)";
                }
                if (res.mismatches == 0) {
                    stream << " (OK)\n";
                } else if (res.first_diff_index) {
                    stream << " first_diff@" << *res.first_diff_index;
                    if (res.host_value && res.ref_value) {
                        stream << " host=" << *res.host_value << " ref=" << *res.ref_value;
                    }
                    stream << '\n';
                } else if (res.host_count > 0 && res.ref_count == 0) {
                    stream << " reference empty\n";
                } else {
                    stream << " length mismatch\n";
                }
            }
        }

        report.total_mismatches += res.mismatches;
        if (res.reference_missing) {
            report.any_missing = true;
        }

        summary << ' ' << res.label << '=' << build_stage_summary_token(res);
        ci << ' ' << res.label << '=' << res.mismatches << '/' << res.host_count;
    }

    report.summary_line = summary.str();
    report.ci_line = ci.str();
    return report;
}

void write_summary_json(const std::filesystem::path& path, const SummaryReport& report)
{
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Failed to open summary output file: " + path.string());
    }

    std::vector<std::string> fields;
    if (report.capture_path) {
        fields.push_back("  \"capture\": \"" + json_escape(*report.capture_path) + "\"");
    }
    if (report.metadata) {
        const auto& meta = *report.metadata;
        std::vector<std::string> meta_fields;
        meta_fields.push_back("    \"sf\": " + std::to_string(meta.sf));
        meta_fields.push_back("    \"bw\": " + std::to_string(meta.bw));
        meta_fields.push_back("    \"sample_rate\": " + std::to_string(meta.sample_rate));
        meta_fields.push_back("    \"cr\": " + std::to_string(meta.cr));
        meta_fields.push_back("    \"payload_len\": " + std::to_string(meta.payload_len));
        meta_fields.push_back("    \"preamble_len\": " + std::to_string(meta.preamble_len));
        meta_fields.push_back("    \"has_crc\": " + std::string(json_bool(meta.has_crc)));

        std::ostringstream meta_ss;
        meta_ss << "  \"metadata\": {\n";
        for (std::size_t i = 0; i < meta_fields.size(); ++i) {
            meta_ss << meta_fields[i];
            if (i + 1 < meta_fields.size()) {
                meta_ss << ',';
            }
            meta_ss << '\n';
        }
        meta_ss << "  }";
        fields.push_back(meta_ss.str());
    }
    if (report.stats) {
        const auto& stats = *report.stats;
        std::ostringstream stats_ss;
        stats_ss << "  \"stats\": {\n"
                 << "    \"sample_count\": " << stats.sample_count << ",\n"
                 << "    \"min_magnitude\": " << stats.min_magnitude << ",\n"
                 << "    \"max_magnitude\": " << stats.max_magnitude << ",\n"
                 << "    \"mean_power\": " << stats.mean_power << "\n"
                 << "  }";
        fields.push_back(stats_ss.str());
    }
    fields.push_back("  \"reference_mismatches\": " + std::to_string(report.reference_mismatches));
    fields.push_back("  \"stage_mismatches\": " + std::to_string(report.stage_mismatches));
    fields.push_back("  \"whitening_roundtrip_ok\": " + std::string(json_bool(report.whitening_roundtrip_ok)));
    fields.push_back("  \"packet_error_rate\": " + std::to_string(report.packet_error_rate));
    fields.push_back("  \"bit_error_rate\": " + std::to_string(report.bit_error_rate));
    fields.push_back("  \"deadline_miss_count\": " + std::to_string(report.deadline_miss_count));
    fields.push_back("  \"tracking_failure\": " + std::string(json_bool(report.tracking_failure)));
    if (!report.tracking_failure_reason.empty()) {
        fields.push_back("  \"tracking_failure_reason\": \""
                         + json_escape(report.tracking_failure_reason) + "\"");
    }
    if (!report.tracking_mitigation.empty()) {
        fields.push_back("  \"tracking_mitigation\": \""
                         + json_escape(report.tracking_mitigation) + "\"");
    }
    if (!report.preview_symbols.empty()) {
        std::ostringstream preview_ss;
        preview_ss << "  \"preview_symbols\": [";
        for (std::size_t i = 0; i < report.preview_symbols.size(); ++i) {
            preview_ss << report.preview_symbols[i];
            if (i + 1 < report.preview_symbols.size()) {
                preview_ss << ", ";
            }
        }
        preview_ss << "]";
        fields.push_back(preview_ss.str());
    }
    if (!report.stage_results.empty()) {
        std::ostringstream stages_ss;
        stages_ss << "  \"stage_results\": [\n";
        for (std::size_t i = 0; i < report.stage_results.size(); ++i) {
            const auto& res = report.stage_results[i];
            std::vector<std::string> stage_fields;
            stage_fields.push_back("      \"label\": \"" + json_escape(res.label) + "\"");
            stage_fields.push_back("      \"host\": " + std::to_string(res.host_count));
            stage_fields.push_back("      \"ref\": " + std::to_string(res.ref_count));
            stage_fields.push_back("      \"mismatches\": " + std::to_string(res.mismatches));
            stage_fields.push_back("      \"summary\": \""
                                   + json_escape(build_stage_summary_token(res)) + "\"");
            stage_fields.push_back("      \"reference_missing\": "
                                   + std::string(json_bool(res.reference_missing)));
            if (res.alignment_offset) {
                stage_fields.push_back("      \"alignment_offset\": "
                                       + std::to_string(*res.alignment_offset));
            }
            stage_fields.push_back("      \"alignment_relative_to_reference\": "
                                   + std::string(json_bool(res.alignment_relative_to_reference)));
            if (res.first_diff_index) {
                stage_fields.push_back("      \"first_diff_index\": "
                                       + std::to_string(*res.first_diff_index));
            }
            if (res.host_value) {
                stage_fields.push_back("      \"host_value\": " + std::to_string(*res.host_value));
            }
            if (res.ref_value) {
                stage_fields.push_back("      \"ref_value\": " + std::to_string(*res.ref_value));
            }

            stages_ss << "    {\n";
            for (std::size_t j = 0; j < stage_fields.size(); ++j) {
                stages_ss << stage_fields[j];
                if (j + 1 < stage_fields.size()) {
                    stages_ss << ',';
                }
                stages_ss << '\n';
            }
            stages_ss << "    }";
            if (i + 1 < report.stage_results.size()) {
                stages_ss << ',';
            }
            stages_ss << '\n';
        }
        stages_ss << "  ]";
        fields.push_back(stages_ss.str());
    }

    {
        std::ostringstream inst_ss;
        if (report.stage_instrumentation.empty()) {
            inst_ss << "  \"stage_instrumentation\": []";
        } else {
            inst_ss << "  \"stage_instrumentation\": [\n";
            for (std::size_t i = 0; i < report.stage_instrumentation.size(); ++i) {
                const auto& entry = report.stage_instrumentation[i];
                inst_ss << "    {\n"
                        << "      \"label\": \"" << json_escape(entry.label) << "\",\n"
                        << "      \"index\": " << entry.index << ",\n"
                        << "      \"avg_ns\": " << entry.avg_ns << ",\n"
                        << "      \"max_ns\": " << entry.max_ns << ",\n"
                        << "      \"avg_cycles\": " << entry.avg_cycles << ",\n"
                        << "      \"max_cycles\": " << entry.max_cycles << ",\n"
                        << "      \"max_scratch_bytes\": " << entry.max_scratch_bytes << "\n"
                        << "    }";
                if (i + 1 < report.stage_instrumentation.size()) {
                    inst_ss << ',';
                }
                inst_ss << '\n';
            }
            inst_ss << "  ]";
        }
        fields.push_back(inst_ss.str());
    }

    out << "{\n";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        out << fields[i];
        if (i + 1 < fields.size()) {
            out << ',';
        }
        out << '\n';
    }
    out << "}\n";
}

std::vector<StageComparisonResult> compare_with_reference(const StageOutputs& outputs,
                                                          const std::filesystem::path& compare_root)
{
    std::filesystem::path base = compare_root;
    if (base.extension() == ".cf32") {
        base.replace_extension("");
    }

    std::vector<StageComparisonResult> results;
    auto compare_stage_file = [&](const char* suffix, const auto& host_vec, const char* label) {
        std::filesystem::path path = base;
        path += suffix;
        if (!std::filesystem::exists(path)) {
            StageComparisonResult missing;
            missing.label = label;
            missing.host_count = host_vec.size();
            missing.ref_count = 0;
            missing.mismatches = host_vec.size();
            missing.reference_missing = true;
            results.push_back(missing);
            return;
        }
        auto reference = read_stage_file(path);
        results.push_back(compare_stage(label, host_vec, reference));
    };

    compare_stage_file("_fft.txt", outputs.fft, "FFT");
    compare_stage_file("_gray.txt", outputs.gray, "Gray");
    compare_stage_file("_deinterleaver.txt", outputs.deinterleaver, "Deinterleaver");
    compare_stage_file("_hamming.txt", outputs.hamming, "Hamming");

    return results;
}

} // namespace host_sim::lora_replay
