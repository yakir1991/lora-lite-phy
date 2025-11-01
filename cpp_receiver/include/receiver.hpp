#pragma once

#include "frame_sync.hpp"
#include "header_decoder.hpp"
#include "iq_loader.hpp"
#include "sync_word_detector.hpp"
#include "payload_decoder.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <vector>

namespace lora {

struct DecodeParams {
    // Spreading factor (5..12). Controls chips per symbol K=2^SF.
    int sf = 7;
    // Bandwidth in Hz (e.g., 125000).
    int bandwidth_hz = 125000;
    // Complex sample rate in Hz. Must be an integer multiple of bandwidth.
    int sample_rate_hz = 500000;
    // LDRO control: 0=off, 1=on, 2=auto.
    enum class LdroMode : std::uint8_t { Off = 0, On = 1, Auto = 2 };
    LdroMode ldro_mode = LdroMode::Auto;
    // LoRa 8-bit sync word (two 4-bit nibbles).
    unsigned sync_word = 0x12;
    // Skip validating the sync word (faster iterations / unknown sync).
    bool skip_sync_word_check = false;
    // Use implicit header mode: do not demodulate header, use provided fields below.
    bool implicit_header = false;
    // Required when implicit_header=true: payload length in bytes (>0).
    int implicit_payload_length = 0;
    // Required when implicit_header=true: whether payload includes CRC16.
    bool implicit_has_crc = true;
    // Required when implicit_header=true: code rate 1..4.
    int implicit_cr = 1;
    // Emit payload bytes incrementally when streaming.
    bool emit_payload_bytes = false;

    // Diagnostics: when non-empty, write a cf32 slice around the header
    // (preamble end through header symbols) to this file path during streaming
    // header decode attempts.
    std::string dump_header_iq_path;
    // When dumping header IQ, also include additional payload symbols after
    // the header to aid external decoders (default: 64 symbols).
    int dump_header_iq_payload_syms = 64;
    // If set, dump the header slice even when header decode fails. The slice
    // will be centered around the estimated header window using the current
    // sync candidate parameters.
    bool dump_header_iq_always = false;
    // Diagnostics: when true, try a small sweep of CFO offsets around the
    // synchronizer estimate during header decode to improve robustness.
    bool header_cfo_sweep = false;
    // When CFO sweep is enabled, sweep ±header_cfo_range_hz in increments of
    // header_cfo_step_hz around the synchronizer CFO estimate.
    double header_cfo_range_hz = 100.0;
    double header_cfo_step_hz = 50.0;
    // Preserve likelihood information across demod/decoding stages instead of
    // collapsing immediately to hard decisions.
    bool soft_decoding = false;
    // Threshold used when ldro_mode==Auto (datasheet recommends 16 ms).
    double ldro_auto_threshold_seconds = 0.016;

    // Sample-rate correction controls. When enabled, downstream decoders will
    // advance through the capture using the caller-provided `sample_rate_ratio`.
    // When disabled, decoders use the synchronizer's internal estimate.
    bool enable_sample_rate_correction = false;
    double sample_rate_ratio = 1.0;
    // Threshold (ppm) beyond which automatic resampling is triggered. Set to 0 to disable.
    double sample_rate_resample_threshold_ppm = 5.0;

    [[nodiscard]] bool ldro_enabled_for_payload() const {
        switch (ldro_mode) {
        case LdroMode::Off:
            return false;
        case LdroMode::On:
            return true;
        case LdroMode::Auto: {
            const double symbol_duration = static_cast<double>(1u << sf) /
                                           static_cast<double>(std::max(bandwidth_hz, 1));
            return symbol_duration >= ldro_auto_threshold_seconds;
        }
        }
        return false;
    }
};

    struct DecodeResult {
        // True when full decode succeeded (payload CRC16 passed if present/expected).
        bool success = false;
        // True if preamble/frame sync succeeded.
        bool frame_synced = false;
        // True if header decode/validation succeeded (explicit mode only).
        bool header_ok = false;
        // True if payload CRC16 verified.
        bool payload_crc_ok = false;
        // Decoded payload bytes (message only).
        std::vector<unsigned char> payload;
        // Demodulated payload raw symbol bins (for debugging/analysis).
        std::vector<int> raw_payload_symbols;
    // Additional instrumentation: payload symbol bins after DE scaling and Gray decode.
    std::vector<int> payload_symbol_bins;
    std::vector<int> payload_degray_values;
        // Fine-aligned start index (samples) estimated by the synchronizer.
        std::ptrdiff_t p_ofs_est = 0;
        // Payload length parsed from the header (explicit) or implicit param.
        int header_payload_length = 0;
        // Effective LDRO state used during payload demodulation.
        bool ldro_used = false;
        // Sample-rate ratio applied after any resampling adjustments.
        double sample_rate_ratio_used = 1.0;
        // Instrumentation: number of sample-rate scan attempts/successes.
        int sr_scan_attempts = 0;
        int sr_scan_successes = 0;
        // Instrumentation: number of CFO sweep trials/successes across header/payload searches.
        int cfo_sweep_attempts = 0;
        int cfo_sweep_successes = 0;
        // Instrumentation: number of payload retries (e.g., resample-based retries).
        int payload_retry_attempts = 0;
        // Whether a cached sample-rate ratio hint was reused for this frame.
        bool used_cached_sample_rate = false;
        // Profiling instrumentation (microseconds).
        double sync_time_us = 0.0;
        double header_time_us = 0.0;
        double payload_time_us = 0.0;
        double resample_time_us = 0.0;
        double retry_time_us = 0.0;
        // CFO / sample-rate diagnostics.
        double cfo_initial_hz = 0.0;
        double cfo_final_hz = 0.0;
        double sample_rate_ratio_initial = 1.0;
        double sample_rate_error_ppm = 0.0;
        double sample_rate_error_initial_ppm = 0.0;
        double sample_rate_drift_per_symbol = 0.0;
        // Streaming instrumentation (per-chunk timings).
        int chunk_count = 0;
        double chunk_time_total_us = 0.0;
        double chunk_time_avg_us = 0.0;
        double chunk_time_min_us = 0.0;
        double chunk_time_max_us = 0.0;
    };

class Receiver {
public:
    // Construct the high-level LoRa receiver with decoding parameters.
    explicit Receiver(const DecodeParams &params);

    // Decode from an in-memory vector of complex samples.
    [[nodiscard]] DecodeResult decode_samples(const std::vector<IqLoader::Sample> &samples) const;
    // Convenience: load cf32 from disk and decode.
    [[nodiscard]] DecodeResult decode_file(const std::filesystem::path &path) const;

    // Diagnostics: last sample-rate ratio used during decode_samples().
    [[nodiscard]] double last_sample_rate_ratio() const { return last_sample_rate_ratio_; }

private:
    [[nodiscard]] std::optional<HeaderDecodeResult> search_header(const std::vector<IqLoader::Sample> &samples,
                                                                  FrameSyncResult &sync) const;
    [[nodiscard]] std::optional<PayloadDecodeResult> search_payload(const std::vector<IqLoader::Sample> &samples,
                                                                    FrameSyncResult &sync,
                                                                    const HeaderDecodeResult &header) const;
    [[nodiscard]] std::vector<IqLoader::Sample> build_resampled_capture(std::span<const IqLoader::Sample> samples,
                                                                        double sample_rate_ratio) const;
    [[nodiscard]] std::vector<double> build_sample_rate_candidates(const FrameSyncResult &sync,
                                                                   std::optional<double> extra_ratio) const;
    [[nodiscard]] std::optional<PayloadDecodeResult> search_payload_with_sample_rates(const std::vector<IqLoader::Sample> &samples,
                                                                                      FrameSyncResult &sync,
                                                                                      const HeaderDecodeResult &header,
                                                                                      std::optional<double> extra_ratio) const;
    struct BruteForceHeaderOutcome {
        HeaderDecodeResult header;
        PayloadDecodeResult payload;
        FrameSyncResult sync;
    };
    [[nodiscard]] std::optional<BruteForceHeaderOutcome> brute_force_header_and_payload(const std::vector<IqLoader::Sample> &samples,
                                                                                        const FrameSyncResult &sync) const;

    DecodeParams params_;
    FrameSynchronizer frame_sync_;
    HeaderDecoder header_decoder_;
    PayloadDecoder payload_decoder_;
    SyncWordDetector sync_detector_;
    mutable double last_sample_rate_ratio_ = 1.0;
};

} // namespace lora
