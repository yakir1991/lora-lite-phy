#include "frame_sync.hpp"
#include "header_decoder.hpp"
#include "iq_loader.hpp"
#include "payload_decoder.hpp"
#include "preamble_detector.hpp"
#include "receiver.hpp"
#include "sync_word_detector.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace {

// This file provides a minimal, dependency-free test harness for the C++ LoRa receiver stages.
// It validates the pipeline against a known-good vector by running stage-by-stage checks:
//  - IQ loader: format, magnitudes, and first samples
//  - Preamble detector: offset and quality metric
//  - Sync word: preamble bins and nibble-coded sync symbols
//  - Frame sync: fine timing offset and CFO estimate
//  - Header decoder: FCS, payload length, CRC flag, CR, and raw K-bins
//  - Payload decoder: bytes and CRC16
//  - Full receiver: integration of all the above
// The intent is fast feedback during development without bringing in a full testing framework.

// Resolve the repository root configured by CMake to locate test vectors.
std::filesystem::path source_root() {
    static const std::filesystem::path root = std::filesystem::path(LORA_CPP_RECEIVER_SOURCE_DIR);
    return root;
}

// Location of example vectors used in the tests.
std::filesystem::path vector_root() {
    static const std::filesystem::path root = source_root().parent_path();
    return root / "vectors";
}

// Tiny assertion helper to avoid bringing in a full unit test dependency.
void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Helpers for approximate equality checks with configurable tolerance.
bool approx(float lhs, float rhs, float eps = 1e-5f) {
    return std::fabs(lhs - rhs) <= eps;
}

bool approx_double(double lhs, double rhs, double eps = 1e-5) {
    return std::fabs(lhs - rhs) <= eps;
}

bool approx_complex(const std::complex<float> &lhs, const std::complex<float> &rhs, float eps = 1e-5f) {
    return approx(lhs.real(), rhs.real(), eps) && approx(lhs.imag(), rhs.imag(), eps);
}

// Load the canonical reference capture used by all baseline checks.
const auto &load_reference_samples() {
    static const auto samples = lora::IqLoader::load_cf32(
        vector_root() / "sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown");
    return samples;
}

// Validate basic IQ loading invariants: sample count, first few samples, and amplitude stats.
void test_iq_loader() {
    const auto &samples = load_reference_samples();

    require(samples.size() == 78080, "Unexpected sample count");

    const std::complex<float> expected_first[]{
        {1.0000000f, 0.0f},
        {0.70819056f, -0.70602125f},
        {0.00613586f, -0.99998116f},
        {-0.69727755f, -0.71680129f},
        {-0.99969882f, -0.02454121f},
    };

    for (std::size_t i = 0; i < std::size(expected_first); ++i) {
        require(approx_complex(samples[i], expected_first[i], 1e-4f), "First samples mismatch");
    }

    float max_mag = 0.0f;
    float sum_mag = 0.0f;
    for (const auto &sample : samples) {
        const float mag = std::abs(sample);
        max_mag = std::max(max_mag, mag);
        sum_mag += mag;
    }
    const float mean_mag = sum_mag / static_cast<float>(samples.size());

    require(approx(max_mag, 1.0f, 1e-5f), "Unexpected max magnitude");
    require(approx(mean_mag, 0.737705f, 1e-4f), "Unexpected mean magnitude");
}

// Check preamble detection: for this vector, we expect a start at 0 and a near-ideal metric.
void test_preamble_detector() {
    const auto &samples = load_reference_samples();

    lora::PreambleDetector detector(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    require(detector.samples_per_symbol() == 512, "Expected 512 samples per symbol");

    const auto result = detector.detect(samples);
    require(result.has_value(), "Expected preamble detection result");
    require(result->offset == 0, "Preamble should start at offset 0 for the reference vector");
    require(approx(result->metric, 1.0f, 1e-3f), "Unexpected preamble detection metric");
}

// Verify sync word detection and preamble bin normalization.
void test_sync_word_detector() {
    const auto &samples = load_reference_samples();

    lora::PreambleDetector pre_detector(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    const auto pre = pre_detector.detect(samples);
    require(pre.has_value(), "Preamble detection must succeed before sync analysis");

    // Estimate CFO to improve magnitude coherence for sync analysis.
    lora::FrameSynchronizer fs(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    const auto fs_res = fs.synchronize(samples);
    require(fs_res.has_value(), "Frame sync must succeed to obtain CFO estimate");

    lora::SyncWordDetector sync_detector(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000, /*sync_word=*/0x12);
    const auto sync = sync_detector.analyze(samples, pre->offset, fs_res->cfo_hz);
    require(sync.has_value(), "Expected sync detection result");

    require(sync->preamble_ok, "Expected clean all-zero preamble symbols");
    require(sync->sync_ok, "Expected sync symbols to match sync word 0x12");

    const std::vector<int> expected_bins = {0, 0, 0, 0, 0, 0, 0, 0, 8, 16};
    require(sync->symbol_bins.size() == expected_bins.size(), "Unexpected symbol bin count");
    for (std::size_t i = 0; i < expected_bins.size(); ++i) {
        require(sync->symbol_bins[i] == expected_bins[i], "Unexpected symbol bin value");
    }

    // Magnitude check: be robust to overall scale (front-end gain, CFO, windowing).
    // Normalize by the average of the preamble magnitudes and validate expected ratios:
    //  - Preamble symbols ~ 1.0
    //  - First sync symbol ~ 480/512 = 0.9375
    //  - Second sync symbol ~ 448/512 = 0.875
    require(sync->magnitudes.size() == expected_bins.size(), "Unexpected magnitude count");
    double pre_sum = 0.0;
    for (std::size_t i = 0; i < 8; ++i) pre_sum += sync->magnitudes[i];
    const double pre_avg = pre_sum / 8.0;
    require(pre_avg > 0.0, "Zero preamble magnitude average");

    for (std::size_t i = 0; i < 8; ++i) {
        const double norm = sync->magnitudes[i] / pre_avg;
        require(approx_double(norm, 1.0, 0.03), "Unexpected preamble magnitude ratio"); // within 3%
    }
    const double sync1_ratio = sync->magnitudes[8] / pre_avg;
    const double sync2_ratio = sync->magnitudes[9] / pre_avg;
    require(approx_double(sync1_ratio, 0.9375, 0.03), "Unexpected first sync magnitude ratio");
    require(approx_double(sync2_ratio, 0.8750, 0.03), "Unexpected second sync magnitude ratio");
}

// Validate frame synchronization outputs: fine timing offset and CFO estimate.
void test_frame_sync() {
    const auto &samples = load_reference_samples();

    lora::FrameSynchronizer sync(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    const auto result = sync.synchronize(samples);
    require(result.has_value(), "Expected frame sync result");
    require(result->p_ofs_est == -25, "Unexpected p_ofs_est");
    require(approx_double(result->cfo_hz, -244.140625, 1e-3), "Unexpected CFO estimate");
}

// Check explicit header decode: FCS, payload length, CRC flag, CR, and raw K-bins.
void test_header_decoder() {
    const auto &samples = load_reference_samples();

    lora::FrameSynchronizer sync(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    const auto sync_res = sync.synchronize(samples);
    require(sync_res.has_value(), "Frame sync required for header decode");

    lora::HeaderDecoder decoder(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    const auto header = decoder.decode(samples, *sync_res);
    require(header.has_value(), "Expected header decode result");
    require(header->fcs_ok, "Header FCS should be valid");
    require(header->payload_length == 18, "Unexpected payload length");
    require(header->has_crc, "Expected CRC flag");
    require(header->cr == 2, "Unexpected coding rate index");

    const std::vector<int> expected_k = {90, 122, 122, 126, 18, 110, 22, 78};
    require(header->raw_symbols == expected_k, "Header raw symbols mismatch");
}

// Validate payload decoding and CRC16 using header and frame sync results.
void test_payload_decoder() {
    const auto &samples = load_reference_samples();

    lora::FrameSynchronizer sync(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    const auto sync_res = sync.synchronize(samples);
    require(sync_res.has_value(), "Frame sync required for payload decode");

    lora::HeaderDecoder header_decoder(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    const auto header = header_decoder.decode(samples, *sync_res);
    require(header.has_value() && header->fcs_ok, "Header decode must succeed before payload");

    lora::PayloadDecoder payload_decoder(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    const auto payload = payload_decoder.decode(samples, *sync_res, *header, /*ldro_enabled=*/false);
    require(payload.has_value(), "Expected payload decode result");
    require(payload->crc_ok, "Payload CRC should be valid");

    const std::vector<unsigned char> expected_bytes = {
        0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x73, 0x74,
        0x75, 0x70, 0x69, 0x64, 0x20, 0x77, 0x6f, 0x72,
        0x6c, 0x64
    };
    require(payload->bytes == expected_bytes, "Payload bytes mismatch");
}

// End-to-end check using the high-level Receiver facade.
void test_full_receiver() {
    const auto &samples = load_reference_samples();

    lora::DecodeParams params;
    params.sf = 7;
    params.bandwidth_hz = 125000;
    params.sample_rate_hz = 500000;
    params.ldro_enabled = false;

    lora::Receiver receiver(params);
    const auto result = receiver.decode_samples(samples);

    require(result.frame_synced, "Receiver failed to sync");
    require(result.header_ok, "Receiver failed header decode");
    require(result.payload_crc_ok, "Receiver payload CRC mismatch");

    const std::vector<unsigned char> expected_bytes = {
        0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x73, 0x74,
        0x75, 0x70, 0x69, 0x64, 0x20, 0x77, 0x6f, 0x72,
        0x6c, 0x64
    };
    require(result.payload == expected_bytes, "Receiver payload mismatch");
}

} // namespace

int main() {
    try {
        // Run stage-by-stage unit checks first. Any failure throws and is caught below.
        test_iq_loader();
        test_preamble_detector();
        test_sync_word_detector();
        test_frame_sync();
        test_header_decoder();
        test_payload_decoder();
        test_full_receiver();
        // If we reach here, all tests passed. Print per-stage PASS messages.
        std::puts("[PASS] frame sync baseline checks");
        std::puts("[PASS] header decoder baseline checks");
        std::puts("[PASS] payload decoder baseline checks");
        std::puts("[PASS] full receiver baseline checks");
    } catch (const std::exception &ex) {
        std::fprintf(stderr, "[FAIL] %s\n", ex.what());
        return 1;
    }

    // Post-summary PASS messages for remaining basic components.
    std::puts("[PASS] iq_loader baseline checks");
    std::puts("[PASS] preamble detector baseline checks");
    std::puts("[PASS] sync word detector baseline checks");
    return 0;
}
