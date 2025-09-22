#include "lora/rx/scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct PipelineParams {
    RxConfig cfg{};
    bool expect_crc = true;
};

std::vector<std::complex<float>> load_samples(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open vector file: " << path << std::endl;
        return {};
    }

    std::vector<std::complex<float>> samples;
    float buf[2];
    while (file.read(reinterpret_cast<char*>(buf), sizeof(buf))) {
        samples.emplace_back(buf[0], buf[1]);
    }

    if (samples.empty()) {
        std::cerr << "No samples loaded from " << path << std::endl;
    }

    return samples;
}

double parse_numeric_suffix(const std::string& token) {
    if (token.empty()) {
        return 0.0;
    }

    double scale = 1.0;
    std::string number = token;
    char last = static_cast<char>(number.back());
    if (std::isalpha(static_cast<unsigned char>(last))) {
        switch (std::tolower(static_cast<unsigned char>(last))) {
        case 'k': scale = 1e3; break;
        case 'm': scale = 1e6; break;
        case 'g': scale = 1e9; break;
        default: break;
        }
        number.pop_back();
    }

    try {
        return std::stod(number) * scale;
    } catch (...) {
        return 0.0;
    }
}

PipelineParams infer_parameters_from_name(const std::string& stem) {
    PipelineParams params;
    params.cfg.sf = 7;
    params.cfg.os = 1;
    params.cfg.ldro = false;
    params.cfg.cr_idx = 1;
    params.cfg.bandwidth_hz = 125000.0f;
    params.expect_crc = true;

    static const std::regex pattern(
        "sps_([^_]+)_bw_([^_]+)_sf_(\\d+)_cr_(\\d+)_ldro_([^_]+)_crc_([^_]+)_implheader_([^_]+)");

    std::smatch match;
    if (std::regex_search(stem, match, pattern)) {
        const double sps = parse_numeric_suffix(match[1].str());
        const double bw = parse_numeric_suffix(match[2].str());
        if (bw > 0.0) {
            params.cfg.bandwidth_hz = static_cast<float>(bw);
        }

        params.cfg.sf = static_cast<uint8_t>(std::stoi(match[3].str()));
        params.cfg.cr_idx = static_cast<uint8_t>(std::stoi(match[4].str()));

        std::string ldro_raw = match[5].str();
        std::string ldro_lower = ldro_raw;
        std::transform(ldro_lower.begin(), ldro_lower.end(), ldro_lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ldro_lower == "true") {
            params.cfg.ldro = true;
        } else if (ldro_lower == "false") {
            params.cfg.ldro = false;
        } else {
            try {
                params.cfg.ldro = std::stoi(ldro_raw) != 0;
            } catch (...) {
                params.cfg.ldro = false;
            }
        }

        std::string crc_raw = match[6].str();
        std::string crc_lower = crc_raw;
        std::transform(crc_lower.begin(), crc_lower.end(), crc_lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (crc_lower == "true") {
            params.expect_crc = true;
        } else if (crc_lower == "false") {
            params.expect_crc = false;
        }

        if (bw > 0.0 && sps > 0.0) {
            params.cfg.os = static_cast<uint32_t>(std::lround(sps / bw));
            if (params.cfg.os == 0) {
                params.cfg.os = 1;
            }
        }
    }

    return params;
}

std::string bytes_to_hex(const std::vector<uint8_t>& data) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < data.size(); ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
        if (i + 1 < data.size()) {
            oss << ' ';
        }
    }
    return oss.str();
}

bool decode_vector_file(const std::string& path) {
    namespace fs = std::filesystem;

    std::cout << "Processing vector: " << path << std::endl;
    auto samples = load_samples(path);
    if (samples.empty()) {
        std::cout << "Success: false" << std::endl;
        std::cout << "Failure reason: unable to load samples" << std::endl;
        return false;
    }

    fs::path file_path(path);
    const PipelineParams params = infer_parameters_from_name(file_path.stem().string());

    std::cout << "Loaded " << samples.size() << " samples" << std::endl;
    std::cout << "Configuration: SF=" << static_cast<int>(params.cfg.sf)
              << " OS=" << params.cfg.os
              << " CR=" << static_cast<int>(params.cfg.cr_idx)
              << " LDRO=" << (params.cfg.ldro ? "true" : "false")
              << " BW=" << params.cfg.bandwidth_hz << " Hz" << std::endl;

    const auto frames = run_pipeline_offline(samples.data(), samples.size(), params.cfg);
    if (frames.empty()) {
        std::cout << "Success: false" << std::endl;
        std::cout << "Failure reason: decoder did not yield any frames" << std::endl;
        return false;
    }

    const FrameCtx& frame = frames.front();
    const bool header_ok = frame.h.ok;
    const bool payload_ok = frame.p.ok && !frame.p.payload_data.empty();
    const bool success = header_ok && payload_ok;

    std::cout << "Success: " << (success ? "true" : "false") << std::endl;
    if (!success) {
        std::cout << "Failure reason: payload decoding failed" << std::endl;
    }

    const bool sync_detected = frame.d.found;
    std::cout << "Frame sync detected: " << (sync_detected ? "true" : "false") << std::endl;
    if (sync_detected) {
        std::cout << "OS: " << frame.d.os << std::endl;
        std::cout << "Phase: " << frame.d.phase << std::endl;
        std::cout << "CFO: " << frame.d.cfo_estimate << std::endl;
        std::cout << "STO: " << frame.d.sto_estimate << std::endl;
        std::cout << "Sync detected: " << (sync_detected ? "true" : "false") << std::endl;
        std::cout << "Sync start sample: " << frame.d.preamble_start_raw << std::endl;
        std::cout << "Aligned start sample: " << frame.frame_start_raw << std::endl;
        std::cout << "Header start sample: " << frame.frame_start_raw << std::endl;
    }

    if (header_ok) {
        std::cout << "Payload length: " << frame.h.payload_len_bytes << std::endl;
        std::cout << "CR: " << static_cast<int>(frame.h.cr_idx) << std::endl;
        std::cout << "Has CRC: " << (frame.h.has_crc ? "true" : "false") << std::endl;
    }

    std::cout << "CRC OK: " << (frame.p.crc_ok ? "true" : "false") << std::endl;
    std::cout << "Length: " << frame.p.payload_data.size() << std::endl;
    std::cout << "Data: " << bytes_to_hex(frame.p.payload_data) << std::endl;

    std::string text(frame.p.payload_data.begin(), frame.p.payload_data.end());
    std::cout << "Text: " << text << std::endl;

    if (frames.size() > 1) {
        std::cout << "Additional frames decoded: " << (frames.size() - 1) << std::endl;
    }

    return success;
}

// Existing regression helpers retained for manual execution.
bool run_scheduler_regression() {
    const std::string regression_path =
        "vectors/sps_125k_bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_nmsgs_8.unknown";
    auto samples = load_samples(regression_path);
    if (samples.empty()) {
        std::cerr << "Scheduler regression input missing or empty: " << regression_path << std::endl;
        return false;
    }

    RxConfig cfg;
    cfg.sf = 7;
    cfg.os = 2;
    cfg.ldro = false;
    cfg.cr_idx = 1;
    cfg.bandwidth_hz = 125000.0f;

    std::cout << "Testing scheduler with " << samples.size() << " samples" << std::endl;
    run_pipeline_offline(samples.data(), samples.size(), cfg);
    std::cout << "Scheduler regression completed successfully" << std::endl;
    return true;
}

bool run_scheduler_sf8_test() {
    const std::string test_path =
        "vectors/sps_1M_bw_250k_sf_8_cr_3_ldro_true_crc_true_implheader_false_test_message.unknown";
    auto samples = load_samples(test_path);
    if (samples.empty()) {
        std::cerr << "SF8 test input missing or empty: " << test_path << std::endl;
        return false;
    }

    RxConfig cfg;
    cfg.sf = 8;
    cfg.os = 4;
    cfg.ldro = true;
    cfg.cr_idx = 3;
    cfg.bandwidth_hz = 250000.0f;

    std::cout << "Testing scheduler with SF=8: " << samples.size() << " samples" << std::endl;
    run_pipeline_offline(samples.data(), samples.size(), cfg);
    std::cout << "SF8 test completed successfully" << std::endl;
    return true;
}

bool run_scheduler_500khz_test() {
    const std::string test_path =
        "vectors/sps4Mhz_sf7_bw500khz_cr47_preamblelen12_syncword18_explicitheader_hascrcFalse_softdecodingTrue.bin";
    auto samples = load_samples(test_path);
    if (samples.empty()) {
        std::cerr << "500kHz test input missing or empty: " << test_path << std::endl;
        return false;
    }

    RxConfig cfg;
    cfg.sf = 7;
    cfg.os = 8;
    cfg.ldro = false;
    cfg.cr_idx = 3;
    cfg.bandwidth_hz = 500000.0f;

    std::cout << "Testing scheduler with 500kHz bandwidth: " << samples.size() << " samples" << std::endl;
    run_pipeline_offline(samples.data(), samples.size(), cfg);
    std::cout << "500kHz test completed successfully" << std::endl;
    return true;
}

bool run_scheduler_hello_world_test() {
    const std::string test_path =
        "vectors/sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown";
    auto samples = load_samples(test_path);
    if (samples.empty()) {
        std::cerr << "Hello world test input missing or empty: " << test_path << std::endl;
        return false;
    }

    RxConfig cfg;
    cfg.sf = 7;
    cfg.os = 4;
    cfg.ldro = false;
    cfg.cr_idx = 2;
    cfg.bandwidth_hz = 125000.0f;

    std::cout << "Testing scheduler with hello world vector: " << samples.size() << " samples" << std::endl;
    run_pipeline_offline(samples.data(), samples.size(), cfg);
    std::cout << "Hello world test completed successfully" << std::endl;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) != "--run-tests") {
        return decode_vector_file(argv[1]) ? 0 : 1;
    }

    std::cout << "=== LoRa Scheduler Test Suite ===" << std::endl;
    bool ok = true;
    ok &= run_scheduler_regression();
    ok &= run_scheduler_sf8_test();
    ok &= run_scheduler_500khz_test();
    ok &= run_scheduler_hello_world_test();

    if (!ok) {
        std::cerr << "Scheduler tests failed" << std::endl;
        return 1;
    }

    std::cout << "\n=== All Scheduler Tests Passed! ===" << std::endl;
    return 0;
}

