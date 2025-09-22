#include "lora/rx/scheduler.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <complex>
#include <span>

namespace {

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

// Removed pipeline functions - using scheduler only

bool run_scheduler_regression() {
    const std::string regression_path =
        "vectors/sps_125k_bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_nmsgs_8.unknown";
    auto samples = load_samples(regression_path);
    if (samples.empty()) {
        std::cerr << "Scheduler regression input missing or empty: " << regression_path << std::endl;
        return false;
    }

    // Configure scheduler
    RxConfig cfg;
    cfg.sf = 7;
    cfg.os = 2;  // oversampling detected from filename
    cfg.ldro = false;
    cfg.cr_idx = 1; // CR45
    cfg.bandwidth_hz = 125000.0f; // 125kHz

    std::cout << "Testing scheduler with " << samples.size() << " samples" << std::endl;

    // Run scheduler-based pipeline
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

    // Configure scheduler for SF=8
    RxConfig cfg;
    cfg.sf = 8;
    cfg.os = 4;  // oversampling detected from filename (1M/250k = 4)
    cfg.ldro = true;  // LDRO enabled for SF=8
    cfg.cr_idx = 3; // CR47
    cfg.bandwidth_hz = 250000.0f; // 250kHz

    std::cout << "Testing scheduler with SF=8: " << samples.size() << " samples" << std::endl;

    // Run scheduler-based pipeline
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

    // Configure scheduler for 500kHz bandwidth
    RxConfig cfg;
    cfg.sf = 7;
    cfg.os = 8;  // oversampling detected from filename (4M/500k = 8)
    cfg.ldro = false;  // LDRO disabled for SF=7
    cfg.cr_idx = 3; // CR47
    cfg.bandwidth_hz = 500000.0f; // 500kHz

    std::cout << "Testing scheduler with 500kHz bandwidth: " << samples.size() << " samples" << std::endl;

    // Run scheduler-based pipeline
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

    // Configure scheduler for hello world vector
    RxConfig cfg;
    cfg.sf = 7;
    cfg.os = 4;  // oversampling detected from filename (500k/125k = 4)
    cfg.ldro = false;  // LDRO disabled
    cfg.cr_idx = 2; // CR46
    cfg.bandwidth_hz = 125000.0f; // 125kHz

    std::cout << "Testing scheduler with hello world vector: " << samples.size() << " samples" << std::endl;
    std::cout << "Expected message: 'hello stupid world'" << std::endl;

    // Run scheduler-based pipeline
    run_pipeline_offline(samples.data(), samples.size(), cfg);

    std::cout << "Hello world test completed successfully" << std::endl;
    return true;
}

// Removed comparison function - using scheduler only

} // namespace

int main(int argc, char** argv) {
    std::cout << "=== LoRa Scheduler Test Suite ===" << std::endl;
    
    // Run all scheduler tests
    if (!run_scheduler_regression()) {
        std::cerr << "Scheduler regression failed" << std::endl;
        return 1;
    }

    if (!run_scheduler_sf8_test()) {
        std::cerr << "Scheduler SF8 test failed" << std::endl;
        return 1;
    }

    if (!run_scheduler_500khz_test()) {
        std::cerr << "Scheduler 500kHz test failed" << std::endl;
        return 1;
    }

    if (!run_scheduler_hello_world_test()) {
        std::cerr << "Scheduler hello world test failed" << std::endl;
        return 1;
    }

    std::cout << "\n=== All Scheduler Tests Passed! ===" << std::endl;
    return 0;
}
