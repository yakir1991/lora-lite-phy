#include "frame_sync.hpp"
#include "header_decoder.hpp"
#include "iq_loader.hpp"
#include "payload_decoder.hpp"
#include "preamble_detector.hpp"
#include "receiver.hpp"
#include "streaming_receiver.hpp"
#include "sync_word_detector.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <algorithm>
#include <span>

// Lightweight smoke tests for the C++ receiver pipeline. They intentionally
// avoid any external unit test framework so that CMake/Ninja builds can run them
// as part of `ctest` without extra dependencies. Each helper checks one stage of
// the pipeline against a reference capture, making it easier to localize
// regressions when tweaking DSP heuristics. Comments annotate the purpose of
// each test and the rationale for key constants and tolerances.

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
// LORA_CPP_RECEIVER_SOURCE_DIR is provided by the CMake build.
std::filesystem::path source_root() {
    static const std::filesystem::path root = std::filesystem::path(LORA_CPP_RECEIVER_SOURCE_DIR);
    return root;
}

// Location of example vectors used in the tests.
// We keep vectors at repo_root/vectors to be shared by Python and C++ tests.
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
// Absolute tolerance comparison for floats; keeps tests robust to minor
// numerical differences between platforms or compiler flags.
bool approx(float lhs, float rhs, float eps = 1e-5f) {
    return std::fabs(lhs - rhs) <= eps;
}

// Same for doubles.
bool approx_double(double lhs, double rhs, double eps = 1e-5) {
    return std::fabs(lhs - rhs) <= eps;
}

// Compare complex values component-wise with a shared tolerance.
bool approx_complex(const std::complex<float> &lhs, const std::complex<float> &rhs, float eps = 1e-5f) {
    return approx(lhs.real(), rhs.real(), eps) && approx(lhs.imag(), rhs.imag(), eps);
}

// Load the canonical reference capture used by all baseline checks.
// The file encodes: sps=500k, bw=125k, sf=7, cr=2, ldro=off, crc=on, payload="hello stupid world".
const auto &load_reference_samples() {
    static const auto samples = lora::IqLoader::load_cf32(
        vector_root() / "sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown");
    return samples;
}

// Validate basic IQ loading invariants: sample count, first few samples, and amplitude stats.
// This guards against file I/O regressions and endian/format issues.
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
// samples_per_symbol should be 512 for SF7 at Fs/BW = 500k/125k = 4, so sps = 128*4 = 512.
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
// Expected bins include 8 and 16 after eight zero preamble bins, which encode sync word 0x12.
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
// The expected values are specific to the reference vector and algorithm tuning.
void test_frame_sync() {
    const auto &samples = load_reference_samples();

    lora::FrameSynchronizer sync(/*sf=*/7, /*bandwidth_hz=*/125000, /*sample_rate_hz=*/500000);
    const auto result = sync.synchronize(samples);
    require(result.has_value(), "Expected frame sync result");
    require(result->p_ofs_est == -25, "Unexpected p_ofs_est");
    require(approx_double(result->cfo_hz, -244.140625, 1e-3), "Unexpected CFO estimate");
}

// Check explicit header decode: FCS, payload length, CRC flag, CR, and raw K-bins.
// Raw symbol values (K-bins) help catch subtle demodulation regressions.
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
// Expected payload bytes correspond to ASCII "hello stupid world".
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
// Verifies the integrated path returns the same expected payload.
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

// Test streaming receiver with various chunk sizes to ensure chunk boundaries
// do not affect correctness or event sequencing.
void test_streaming_receiver_chunking() {
    const auto &samples = load_reference_samples();

    const std::vector<unsigned char> expected_bytes = {
        0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x73, 0x74,
        0x75, 0x70, 0x69, 0x64, 0x20, 0x77, 0x6f, 0x72,
        0x6c, 0x64
    };

    const std::vector<std::size_t> chunk_sizes = {1, 7, 64, 1024};

    for (std::size_t chunk : chunk_sizes) {
        // Configure decode parameters matching the reference vector.
        lora::DecodeParams params;
        params.sf = 7;
        params.bandwidth_hz = 125000;
        params.sample_rate_hz = 500000;
        params.ldro_enabled = false;

        lora::StreamingReceiver streaming(params);
        std::vector<lora::StreamingReceiver::FrameEvent> events;

        std::size_t offset = 0;
        while (offset < samples.size()) {
            // Feed chunks; accumulate all emitted events for later validation.
            const std::size_t take = std::min(chunk, samples.size() - offset);
            std::span<const lora::StreamingReceiver::Sample> span(&samples[offset], take);
            auto ev = streaming.push_samples(span);
            events.insert(events.end(), ev.begin(), ev.end());
            offset += take;
        }

        require(!events.empty(), "Streaming receiver emitted no events");

        const auto sync_it = std::find_if(events.begin(), events.end(), [](const auto &ev) {
            return ev.type == lora::StreamingReceiver::FrameEvent::Type::SyncAcquired;
        });
        require(sync_it != events.end(), "Streaming receiver missing sync event");
        require(sync_it->sync.has_value(), "Sync event missing state");

        const auto err_it = std::find_if(events.begin(), events.end(), [](const auto &ev) {
            return ev.type == lora::StreamingReceiver::FrameEvent::Type::FrameError;
        });
        require(err_it == events.end(), "Streaming receiver reported error");

        const auto done_it = std::find_if(events.begin(), events.end(), [](const auto &ev) {
            return ev.type == lora::StreamingReceiver::FrameEvent::Type::FrameDone;
        });
        require(done_it != events.end(), "Streaming receiver missing FrameDone event");
        require(done_it->result.has_value(), "FrameDone missing decode result");
        require(done_it->result->success, "Streaming decode did not succeed");
        require(done_it->result->payload == expected_bytes, "Streaming payload mismatch");
    }
}

// Build a composite buffer with two frames separated by a gap of zeros and
// verify that the streaming receiver emits two successful FrameDone events.
void test_streaming_receiver_multi_frame() {
    const auto &samples = load_reference_samples();

    const std::vector<unsigned char> expected_bytes = {
        0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x73, 0x74,
        0x75, 0x70, 0x69, 0x64, 0x20, 0x77, 0x6f, 0x72,
        0x6c, 0x64
    };

    const std::size_t gap_symbols = 16;
    const std::size_t gap_samples = gap_symbols * 512; // sps=512 for reference vector

    std::vector<lora::StreamingReceiver::Sample> composite;
    composite.reserve(samples.size() * 2 + gap_samples);
    composite.insert(composite.end(), samples.begin(), samples.end());
    composite.insert(composite.end(), gap_samples, lora::StreamingReceiver::Sample{0.0f, 0.0f});
    composite.insert(composite.end(), samples.begin(), samples.end());

    lora::DecodeParams params;
    params.sf = 7;
    params.bandwidth_hz = 125000;
    params.sample_rate_hz = 500000;
    params.ldro_enabled = false;

    lora::StreamingReceiver streaming(params);

    std::size_t chunk = 321;
    std::size_t offset = 0;
    std::vector<lora::StreamingReceiver::FrameEvent> events;
    while (offset < composite.size()) {
        const std::size_t take = std::min(chunk, composite.size() - offset);
        std::span<const lora::StreamingReceiver::Sample> span(&composite[offset], take);
        auto ev = streaming.push_samples(span);
        events.insert(events.end(), ev.begin(), ev.end());
        offset += take;
    }

    const auto err_it = std::find_if(events.begin(), events.end(), [](const auto &ev) {
        return ev.type == lora::StreamingReceiver::FrameEvent::Type::FrameError;
    });
    require(err_it == events.end(), "Streaming multi-frame reported error");

    std::vector<const lora::StreamingReceiver::FrameEvent *> done_events;
    for (const auto &ev : events) {
        if (ev.type == lora::StreamingReceiver::FrameEvent::Type::FrameDone) {
            done_events.push_back(&ev);
        }
    }
    require(done_events.size() >= 2, "Expected at least two FrameDone events");
    for (const auto *ev : done_events) {
        require(ev->result.has_value(), "FrameDone missing result");
        require(ev->result->success, "FrameDone result not successful");
        require(ev->result->payload == expected_bytes, "Frame payload mismatch in multi-frame test");
    }
}

// Add low-variance AWGN to the reference samples and enable emit_payload_bytes.
// Ensure robustness by verifying progressive byte events match the expected payload.
void test_streaming_receiver_awgn() {
    const auto &samples = load_reference_samples();

    std::vector<lora::StreamingReceiver::Sample> noisy(samples.begin(), samples.end());

    std::mt19937 rng(12345); // fixed seed for deterministic noise
    std::normal_distribution<float> noise(0.0f, 0.01f);
    for (auto &s : noisy) {
        s += lora::StreamingReceiver::Sample{noise(rng), noise(rng)};
    }

    lora::DecodeParams params;
    params.sf = 7;
    params.bandwidth_hz = 125000;
    params.sample_rate_hz = 500000;
    params.ldro_enabled = false;
    params.emit_payload_bytes = true;

    lora::StreamingReceiver streaming(params);

    std::vector<unsigned char> collected_bytes;

    const std::size_t chunk = 321;
    std::size_t offset = 0;
    while (offset < noisy.size()) {
        const std::size_t take = std::min(chunk, noisy.size() - offset);
        std::span<const lora::StreamingReceiver::Sample> span(&noisy[offset], take);
        auto events = streaming.push_samples(span);
        for (const auto &ev : events) {
            if (ev.type == lora::StreamingReceiver::FrameEvent::Type::PayloadByte && ev.payload_byte.has_value()) {
                collected_bytes.push_back(*ev.payload_byte);
            } else if (ev.type == lora::StreamingReceiver::FrameEvent::Type::FrameDone) {
                offset = noisy.size();
                break;
            }
        }
        offset += take;
    }

    const std::vector<unsigned char> expected_bytes = {
        0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x73, 0x74,
        0x75, 0x70, 0x69, 0x64, 0x20, 0x77, 0x6f, 0x72,
        0x6c, 0x64
    };

    require(collected_bytes.size() == expected_bytes.size(), "AWGN streaming: unexpected payload size");
    require(std::equal(collected_bytes.begin(), collected_bytes.end(), expected_bytes.begin()),
            "AWGN streaming: payload mismatch");
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
        test_streaming_receiver_chunking();
        test_streaming_receiver_multi_frame();
        test_streaming_receiver_awgn();
        // If we reach here, all tests passed. Print per-stage PASS messages.
        std::puts("[PASS] frame sync baseline checks");
        std::puts("[PASS] header decoder baseline checks");
        std::puts("[PASS] payload decoder baseline checks");
        std::puts("[PASS] full receiver baseline checks");
        std::puts("[PASS] streaming receiver chunked checks");
        std::puts("[PASS] streaming receiver multi-frame checks");
        std::puts("[PASS] streaming receiver AWGN checks");
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
