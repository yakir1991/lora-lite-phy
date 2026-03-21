#include "host_sim/lora_replay/stage_processing.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>

using namespace host_sim::lora_replay;

namespace
{

std::filesystem::path create_temp_base_path()
{
    static int counter = 0;
    const auto stamp =
        static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    auto dir = std::filesystem::temp_directory_path()
               / ("lora_stage_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
    std::filesystem::create_directories(dir);
    return dir / "capture";
}

void cleanup_stage_files(const std::filesystem::path& base)
{
    static const char* suffixes[] = {"_fft.txt", "_gray.txt", "_deinterleaver.txt", "_hamming.txt"};
    for (const char* suffix : suffixes) {
        std::error_code ec;
        std::filesystem::remove(base.string() + suffix, ec);
    }
    std::error_code ec;
    std::filesystem::remove_all(base.parent_path(), ec);
}

} // namespace

int main()
{
    StageOutputs outputs;
    outputs.fft = {1, 2, 3};
    outputs.gray = {2, 4, 6};
    outputs.deinterleaver = {9, 10};
    outputs.hamming = {0xA, 0xB};

    const auto base = create_temp_base_path();
    write_stage_file(base.string() + "_fft.txt", outputs.fft);
    write_stage_file(base.string() + "_gray.txt", outputs.gray);
    write_stage_file(base.string() + "_deinterleaver.txt", outputs.deinterleaver);
    write_stage_file(base.string() + "_hamming.txt", outputs.hamming);

    auto comparison_results = compare_with_reference(outputs, base);
    if (comparison_results.size() != 4) {
        std::cerr << "Unexpected comparison count: " << comparison_results.size() << "\n";
        cleanup_stage_files(base);
        return 1;
    }
    for (const auto& result : comparison_results) {
        if (result.mismatches != 0) {
            std::cerr << "Stage " << result.label << " mismatch count: " << result.mismatches << "\n";
            cleanup_stage_files(base);
            return 1;
        }
    }

    StageOutputs normalized_test;
    append_fft_gray({1, 5}, false, false, 7, normalized_test);
    if (normalized_test.fft.size() != 2 || normalized_test.gray.size() != 2) {
        std::cerr << "append_fft_gray produced unexpected sizes\n";
        cleanup_stage_files(base);
        return 1;
    }
    if (normalized_test.fft[0] != 0 || normalized_test.gray[0] != 0) {
        std::cerr << "FFT/Gray values incorrect for symbol 1\n";
        cleanup_stage_files(base);
        return 1;
    }

    StageComparisonResult missing;
    missing.label = "FFT";
    missing.reference_missing = true;
    missing.host_count = 3;
    if (build_stage_summary_token(missing) != "missing") {
        std::cerr << "Missing summary token incorrect\n";
        cleanup_stage_files(base);
        return 1;
    }

    auto ok_token = build_stage_summary_token(comparison_results.front());
    if (ok_token != "OK") {
        std::cerr << "Expected OK summary token, got " << ok_token << "\n";
        cleanup_stage_files(base);
        return 1;
    }

    StageComparisonResult failing;
    failing.label = "Gray";
    failing.host_count = 3;
    failing.ref_count = 3;
    failing.mismatches = 2;
    failing.first_diff_index = 1;
    failing.host_value = 7;
    failing.ref_value = 5;
    auto fail_token = build_stage_summary_token(failing);
    if (fail_token.find("FAIL") != 0) {
        std::cerr << "Expected failure summary token, got " << fail_token << "\n";
        cleanup_stage_files(base);
        return 1;
    }

    std::ostringstream silent_stream;
    auto diff_report =
        build_stage_diff_report({comparison_results.front(), missing, failing}, false, silent_stream);
    if (!diff_report.any_missing || diff_report.total_mismatches != (missing.mismatches + failing.mismatches)) {
        std::cerr << "Stage diff report summary incorrect\n";
        cleanup_stage_files(base);
        return 1;
    }

    const std::vector<uint8_t> payload = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    if (compute_lora_crc(payload) != 0x2E90) {
        std::cerr << "CRC check failed\n";
        cleanup_stage_files(base);
        return 1;
    }

    cleanup_stage_files(base);
    return 0;
}
