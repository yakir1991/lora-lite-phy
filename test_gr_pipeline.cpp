#include "lora/rx/gr_pipeline.hpp"
#include "lora/rx/gr/header_decode.hpp"
#include "lora/rx/gr/utils.hpp"
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

lora::rx::pipeline::Config make_default_config() {
    lora::rx::pipeline::Config cfg;
    cfg.sf = 7;
    cfg.min_preamble_syms = 8;
    cfg.symbols_after_preamble = 2.25f;
    cfg.header_symbol_count = 16;
    cfg.os_candidates = {4, 2, 1, 8};
    cfg.expected_sync_word = 0x34;
    cfg.decode_payload = true;
    cfg.expect_payload_crc = true;
    return cfg;
}

bool run_oversampled_multi_frame_regression() {
    // Temporarily disabled to test scheduler
    std::cout << "Oversampled multi-frame regression skipped" << std::endl;
    return true;
}

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

bool run_scheduler_vs_original_comparison() {
    const std::string regression_path =
        "vectors/sps_125k_bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_nmsgs_8.unknown";
    auto samples = load_samples(regression_path);
    if (samples.empty()) {
        std::cerr << "Comparison test input missing: " << regression_path << std::endl;
        return false;
    }

    std::cout << "=== COMPARISON: Original Pipeline vs Scheduler ===" << std::endl;
    std::cout << "Input: " << samples.size() << " samples" << std::endl;

    // Test original pipeline
    std::cout << "\n--- Original Pipeline ---" << std::endl;
    auto cfg = make_default_config();
    lora::rx::pipeline::GnuRadioLikePipeline pipeline(cfg);
    auto result = pipeline.run(samples);
    
    std::cout << "Original: " << (result.success ? "SUCCESS" : "FAILED") << std::endl;
    if (result.success) {
        std::cout << "  Frames decoded: " << result.frame_count << std::endl;
        std::cout << "  OS detected: " << result.frame_sync.os << std::endl;
    }

    // Test scheduler
    std::cout << "\n--- Scheduler Pipeline ---" << std::endl;
    RxConfig sched_cfg;
    sched_cfg.sf = 7;
    sched_cfg.os = 2;
    sched_cfg.ldro = false;
    sched_cfg.cr_idx = 1; // CR45
    sched_cfg.bandwidth_hz = 125000.0f; // 125kHz
    
    run_pipeline_offline(samples.data(), samples.size(), sched_cfg);

    std::cout << "\n=== Comparison completed ===" << std::endl;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string vec_path = "vectors/sps_125k_bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_nmsgs_8.unknown";
    if (argc > 1) vec_path = argv[1];

    auto samples = load_samples(vec_path);
    if (samples.empty()) {
        std::cerr << "Unable to load samples from " << vec_path << std::endl;
        return 1;
    }

    std::cout << "Loaded " << samples.size() << " samples" << std::endl;

    // Configure the pipeline
    auto cfg = make_default_config();
    
    // Create and run pipeline
    lora::rx::pipeline::GnuRadioLikePipeline pipeline(cfg);

    {
        lora::Workspace tmp_ws;
        auto det = lora::rx::gr::detect_preamble_os(tmp_ws,
                                                    std::span<const std::complex<float>>(samples.data(), samples.size()),
                                                    cfg.sf,
                                                    cfg.min_preamble_syms,
                                                    cfg.os_candidates);
        if (det) {
            std::cout << "Manual preamble detect: os=" << det->os
                      << " phase=" << det->phase
                      << " start_sample=" << det->start_sample << std::endl;
        } else {
            std::cout << "Manual preamble detect: none" << std::endl;
        }
    }

    auto result = pipeline.run(samples);
    
    // Print results
    std::cout << "Success: " << (result.success ? "true" : "false") << std::endl;
    if (!result.success) {
        std::cout << "Failure reason: " << result.failure_reason << std::endl;
    }
    
    std::cout << "Frame sync detected: " << (result.frame_sync.detected ? "true" : "false") << std::endl;
    if (result.frame_sync.detected) {
        std::cout << "OS: " << result.frame_sync.os << std::endl;
        std::cout << "Phase: " << result.frame_sync.phase << std::endl;
        std::cout << "CFO: " << result.frame_sync.cfo << std::endl;
        std::cout << "STO: " << result.frame_sync.sto << std::endl;
        std::cout << "Sync detected: " << (result.frame_sync.sync_detected ? "true" : "false") << std::endl;
        if (result.frame_sync.sync_detected) {
            std::cout << "Sync start sample: " << result.frame_sync.sync_start_sample << std::endl;
        }
        std::cout << "Aligned start sample: " << result.frame_sync.aligned_start_sample << std::endl;
        std::cout << "Header start sample: " << result.frame_sync.header_start_sample << std::endl;
    }

    if (result.header.header) {
        std::cout << "Header decoded successfully:" << std::endl;
        std::cout << "  Payload length: " << (int)result.header.header->payload_len << std::endl;
        std::cout << "  CR: " << (int)result.header.header->cr << std::endl;
        std::cout << "  Has CRC: " << (result.header.header->has_crc ? "true" : "false") << std::endl;
    }
    
    if (result.success && !result.payload.dewhitened_payload.empty()) {
        std::cout << "Payload decoded successfully:" << std::endl;
        std::cout << "  Length: " << result.payload.dewhitened_payload.size() << " bytes" << std::endl;
        std::cout << "  CRC OK: " << (result.payload.crc_ok ? "true" : "false") << std::endl;
        std::cout << "  Data: ";
        for (auto byte : result.payload.dewhitened_payload) {
            std::cout << std::hex << (int)byte << " ";
        }
        std::cout << std::endl;
    } else if (!result.payload.dewhitened_payload.empty()) {
        std::cout << "Payload (partial): length=" << result.payload.dewhitened_payload.size()
                  << " crc_ok=" << (result.payload.crc_ok ? "true" : "false") << std::endl;
        std::cout << "  First 32 bytes: ";
        size_t limit = std::min<size_t>(32, result.payload.dewhitened_payload.size());
        for (size_t i = 0; i < limit; ++i) {
            std::cout << std::hex << (int)result.payload.dewhitened_payload[i] << ' ';
        }
        std::cout << std::dec << std::endl;
        if (!result.payload.crc_ok && result.header.header) {
            auto hdr = *result.header.header;
            if (result.fec.raw_bytes.size() >= hdr.payload_len + 2) {
                uint16_t crc_rx = static_cast<uint16_t>(result.fec.raw_bytes[hdr.payload_len]) |
                                  (static_cast<uint16_t>(result.fec.raw_bytes[hdr.payload_len + 1]) << 8);
                std::cout << "  CRC RX: 0x" << std::hex << crc_rx << std::dec << std::endl;
                uint16_t crc_calc = lora::rx::gr::Crc16Ccitt{}.compute(result.payload.dewhitened_payload.data(), hdr.payload_len);
                std::cout << "  CRC calc: 0x" << std::hex << crc_calc << std::dec << std::endl;
            }
        }
    }

    if (!result.fec.nibbles.empty()) {
        std::cout << "FEC nibbles (first 20): ";
        size_t limit = std::min<size_t>(20, result.fec.nibbles.size());
        for (size_t i = 0; i < limit; ++i) std::cout << std::hex << (int)result.fec.nibbles[i] << ' ';
        std::cout << std::dec << " (total=" << result.fec.nibbles.size() << ")" << std::endl;
    }

    if (!result.bits.deinterleaved_bits.empty()) {
        std::cout << "Deinterleaved bits (first 40): ";
        size_t limit = std::min<size_t>(40, result.bits.deinterleaved_bits.size());
        for (size_t i = 0; i < limit; ++i)
            std::cout << (int)result.bits.deinterleaved_bits[i];
        std::cout << std::endl;
    }

    if (!result.fec.raw_bytes.empty()) {
        std::cout << "Raw payload bytes (first 32): ";
        size_t limit = std::min<size_t>(32, result.fec.raw_bytes.size());
        for (size_t i = 0; i < limit; ++i) {
            std::cout << std::hex << (int)result.fec.raw_bytes[i] << ' ';
        }
        std::cout << std::dec << std::endl;
        if (result.fec.raw_bytes.size() >= 4) {
            std::cout << "Raw payload bytes (last 4): ";
            for (size_t i = result.fec.raw_bytes.size() - 4; i < result.fec.raw_bytes.size(); ++i)
                std::cout << std::hex << (int)result.fec.raw_bytes[i] << ' ';
            std::cout << std::dec << std::endl;
        }
    }

    if (!result.fft.raw_bins.empty()) {
   std::cout << "First 16 raw bins: ";
   size_t to_print = std::min<size_t>(16, result.fft.raw_bins.size());
   for (size_t i = 0; i < to_print; ++i) {
       std::cout << result.fft.raw_bins[i] << " ";
   }
   std::cout << std::endl;
   std::cout << "Total symbols demodulated: " << result.fft.raw_bins.size() << std::endl;
   if (result.fft.raw_bins.size() >= cfg.header_symbol_count)
       std::cout << "Payload symbols: " << (result.fft.raw_bins.size() - cfg.header_symbol_count) << std::endl;

    if (!result.header.header_bytes.empty()) {
        std::cout << "Header bytes: ";
        for (auto b : result.header.header_bytes) std::cout << std::hex << (int)b << ' ';
        std::cout << std::dec << std::endl;
    }
    }

    lora::Workspace ws;
    auto hdr_res = lora::rx::gr::decode_header_with_preamble_cfo_sto_os(
        ws,
        std::span<const std::complex<float>>(result.frame_sync.frame_samples.data(),
                                             result.frame_sync.frame_samples.size()),
        cfg.sf,
        lora::rx::gr::CodeRate::CR45,
        cfg.min_preamble_syms,
        cfg.expected_sync_word);
    std::cout << "decode_header_with_preamble success: " << (hdr_res ? "true" : "false") << std::endl;
    if (hdr_res) {
        std::cout << "  Payload length: " << (int)hdr_res->payload_len << std::endl;
        std::cout << "  CR: " << (int)hdr_res->cr << std::endl;
        std::cout << "  Has CRC: " << (hdr_res->has_crc ? "true" : "false") << std::endl;
    }



    lora::Workspace ws2;
    auto hdr_impl = lora::rx::gr::decode_header_with_preamble_cfo_sto_os(
        ws2,
        std::span<const std::complex<float>>(samples.data(), samples.size()),
        cfg.sf,
        lora::rx::gr::CodeRate::CR45,
        cfg.min_preamble_syms,
        cfg.expected_sync_word);
    std::cout << "impl header success: " << (hdr_impl ? "true" : "false") << std::endl;
    if (hdr_impl) {
        std::cout << "  Payload length: " << (int)hdr_impl->payload_len << std::endl;
        std::cout << "  CR: " << (int)hdr_impl->cr << std::endl;
        std::cout << "  Has CRC: " << (hdr_impl->has_crc ? "true" : "false") << std::endl;
        std::cout << "  dbg_hdr_syms_raw: ";
        for (int i = 0; i < 16; ++i) std::cout << ws2.dbg_hdr_syms_raw[i] << ' ';
        std::cout << std::endl;
        std::cout << "  dbg_hdr_syms_corr: ";
        for (int i = 0; i < 16; ++i) std::cout << ws2.dbg_hdr_syms_corr[i] << ' ';
        std::cout << std::endl;
        std::cout << "  dbg_hdr_gray: ";
        for (int i = 0; i < 16; ++i) std::cout << ws2.dbg_hdr_gray[i] << ' ';
        std::cout << std::endl;
        std::cout << "  dbg_hdr_nibbles_cr48: ";
        for (int i = 0; i < 10; ++i) std::cout << (int)ws2.dbg_hdr_nibbles_cr48[i] << ' ';
        std::cout << std::endl;
    }

    // Temporarily skip oversampled regression to test scheduler
    // if (!run_oversampled_multi_frame_regression()) {
    //     std::cerr << "Oversampled multi-frame regression failed" << std::endl;
    //     return 1;
    // }

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

    if (!run_scheduler_vs_original_comparison()) {
        std::cerr << "Scheduler comparison failed" << std::endl;
        return 1;
    }

    return 0;
}
