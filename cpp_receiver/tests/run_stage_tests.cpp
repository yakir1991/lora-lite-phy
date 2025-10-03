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

std::filesystem::path source_root() {
    static const std::filesystem::path root = std::filesystem::path(LORA_CPP_RECEIVER_SOURCE_DIR);
    return root;
}

std::filesystem::path vector_root() {
    static const std::filesystem::path root = source_root().parent_path();
    return root / "vectors";
}

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool approx(float lhs, float rhs, float eps = 1e-5f) {
    return std::fabs(lhs - rhs) <= eps;
}

bool approx_double(double lhs, double rhs, double eps = 1e-5) {
    return std::fabs(lhs - rhs) <= eps;
}

bool approx_complex(const std::complex<float> &lhs, const std::complex<float> &rhs, float eps = 1e-5f) {
    return approx(lhs.real(), rhs.real(), eps) && approx(lhs.imag(), rhs.imag(), eps);
}

const auto &load_reference_samples() {
    static const auto samples = lora::IqLoader::load_cf32(
        vector_root() / "sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown");
    return samples;
}

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

void test_preamble_detector() {
    const auto &samples = load_reference_samples();

    lora::PreambleDetector detector(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    require(detector.samples_per_symbol() == 512, "Expected 512 samples per symbol");

    const auto result = detector.detect(samples);
    require(result.has_value(), "Expected preamble detection result");
    require(result->offset == 0, "Preamble should start at offset 0 for the reference vector");
    require(approx(result->metric, 1.0f, 1e-3f), "Unexpected preamble detection metric");
}

void test_sync_word_detector() {
    const auto &samples = load_reference_samples();

    lora::PreambleDetector pre_detector(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    const auto pre = pre_detector.detect(samples);
    require(pre.has_value(), "Preamble detection must succeed before sync analysis");

    lora::SyncWordDetector sync_detector(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000, /*sync_word=*/0x12);
    const auto sync = sync_detector.analyze(samples, pre->offset);
    require(sync.has_value(), "Expected sync detection result");

    require(sync->preamble_ok, "Expected clean all-zero preamble symbols");
    require(sync->sync_ok, "Expected sync symbols to match sync word 0x12");

    const std::vector<int> expected_bins = {0, 0, 0, 0, 0, 0, 0, 0, 8, 16};
    require(sync->symbol_bins.size() == expected_bins.size(), "Unexpected symbol bin count");
    for (std::size_t i = 0; i < expected_bins.size(); ++i) {
        require(sync->symbol_bins[i] == expected_bins[i], "Unexpected symbol bin value");
    }

    const std::vector<double> expected_mags = {
        512.0000002205599,
        512.0000002205599,
        512.0000002205599,
        512.0000002205599,
        512.0000002205599,
        512.0000002205599,
        512.0000002205599,
        512.0000002205599,
        479.99997309066487,
        448.0000009483169,
    };
    require(sync->magnitudes.size() == expected_mags.size(), "Unexpected magnitude count");
    for (std::size_t i = 0; i < expected_mags.size(); ++i) {
        const double mag = sync->magnitudes[i];
        const double expect = expected_mags[i];
        const double tolerance = (i < 8) ? 1e-3 : 1e-2; // sync chirps have slightly lower peak
        require(approx_double(mag, expect, tolerance), "Unexpected symbol magnitude");
    }
}

void test_frame_sync() {
    const auto &samples = load_reference_samples();

    lora::FrameSynchronizer sync(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    const auto result = sync.synchronize(samples);
    require(result.has_value(), "Expected frame sync result");
    require(result->p_ofs_est == -25, "Unexpected p_ofs_est");
    require(approx_double(result->cfo_hz, -244.140625, 1e-3), "Unexpected CFO estimate");
}

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
        test_iq_loader();
        test_preamble_detector();
        test_sync_word_detector();
        test_frame_sync();
        test_header_decoder();
        test_payload_decoder();
        test_full_receiver();
        std::puts("[PASS] frame sync baseline checks");
        std::puts("[PASS] header decoder baseline checks");
        std::puts("[PASS] payload decoder baseline checks");
        std::puts("[PASS] full receiver baseline checks");
    } catch (const std::exception &ex) {
        std::fprintf(stderr, "[FAIL] %s\n", ex.what());
        return 1;
    }

    std::puts("[PASS] iq_loader baseline checks");
    std::puts("[PASS] preamble detector baseline checks");
    std::puts("[PASS] sync word detector baseline checks");
    return 0;
}
