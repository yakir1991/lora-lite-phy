#pragma once

#include <complex>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace lora {

// Output of the frame synchronizer describing where a potential LoRa frame
// begins in the sample stream and the frequency offset to correct.
struct FrameSyncResult {
    // Coarse preamble start offset in samples (relative to the analyzed buffer).
    // Used as an anchor to compute header/payload regions.
    std::ptrdiff_t preamble_offset = 0;
    // Fine-aligned symbol start in samples (fractional timing accounted for).
    // Header/payload decoders should use this for accurate symbol alignment.
    std::ptrdiff_t p_ofs_est = 0;
    // Estimated carrier frequency offset in Hz. Apply a complex exponential
    // rotation e^{-j2π f_cfo t} before demodulation to mitigate CFO.
    double cfo_hz = 0.0;
};

// Batch frame synchronizer: searches a block of I/Q samples for a LoRa
// preamble, estimates symbol timing (coarse+fine), and CFO. Typical flow:
//  1) Build reference upchirp/downchirp at current SF/BW/FS.
//  2) Slide a window, dechirp, take DFT to find bin peaks consistent with
//     preamble tones.
//  3) Validate preamble structure and refine timing using a fine search.
//  4) Return `FrameSyncResult` or std::nullopt if no reliable detection.
class FrameSynchronizer {
public:
    using Sample = std::complex<float>;

    // Scratch buffers to avoid per-call allocations when embedding the synchronizer.
    struct Scratch {
        std::vector<std::complex<double>> win_up;
        std::vector<std::complex<double>> win_down;
        std::vector<std::complex<double>> spectrum_up;
        std::vector<std::complex<double>> spectrum_down;
        std::vector<std::complex<double>> fine_segment;
        std::vector<std::complex<double>> fine_spectrum;
        std::vector<double> phase_history;

        // Ensure the up-chirp dechirp window is large enough and return mutable access.
        [[nodiscard]] std::vector<std::complex<double>> &ensure_win_up(std::size_t required) {
            if (win_up.size() < required) {
                win_up.resize(required);
            }
            return win_up;
        }

        // Ensure the down-chirp dechirp window is large enough and return mutable access.
        [[nodiscard]] std::vector<std::complex<double>> &ensure_win_down(std::size_t required) {
            if (win_down.size() < required) {
                win_down.resize(required);
            }
            return win_down;
        }

        // Allow reuse of coarse FFT buffers for the up-chirp path.
        [[nodiscard]] std::vector<std::complex<double>> &ensure_spectrum_up(std::size_t required) {
            if (spectrum_up.size() < required) {
                spectrum_up.resize(required);
            }
            return spectrum_up;
        }

        // Allow reuse of coarse FFT buffers for the down-chirp path.
        [[nodiscard]] std::vector<std::complex<double>> &ensure_spectrum_down(std::size_t required) {
            if (spectrum_down.size() < required) {
                spectrum_down.resize(required);
            }
            return spectrum_down;
        }

        // Temporary storage used during fine CFO/timing refinement.
        [[nodiscard]] std::vector<std::complex<double>> &ensure_fine_segment(std::size_t required) {
            if (fine_segment.size() < required) {
                fine_segment.resize(required);
            }
            return fine_segment;
        }

        // FFT buffer for the fine refinement stage.
        [[nodiscard]] std::vector<std::complex<double>> &ensure_fine_spectrum(std::size_t required) {
            if (fine_spectrum.size() < required) {
                fine_spectrum.resize(required);
            }
            return fine_spectrum;
        }

        // Phase-history scratch used by the preamble pattern checker (initialised to -1).
        [[nodiscard]] std::vector<double> &ensure_phase_history(std::size_t required) {
            if (phase_history.size() != required) {
                phase_history.assign(required, -1.0);
            } else {
                std::fill(phase_history.begin(), phase_history.end(), -1.0);
            }
            return phase_history;
        }
    };

    // Construct a LoRa frame synchronizer.
    // Parameters:
    //  - sf: Spreading factor (5..12)
    //  - bandwidth_hz: LoRa bandwidth in Hz (positive)
    //  - sample_rate_hz: complex sample rate in Hz. Must be an integer multiple of BW
    //    so that the oversampling factor os_factor_ = Fs/BW is integral.
    FrameSynchronizer(int sf, int bandwidth_hz, int sample_rate_hz);

    // Find the preamble and estimate timing and CFO from a vector of I/Q samples.
    // Returns:
    //  - FrameSyncResult with coarse preamble_offset, refined p_ofs_est, and cfo_hz,
    //    or std::nullopt if no valid preamble was detected.
    // Notes:
    //  - The implementation uses dechirp + DFT peak search over a sliding window,
    //    validates preamble pattern consistency, then refines timing with a fine search.
    //  - p_ofs_est should be passed to header/payload decoders to ensure symbol alignment.
    [[nodiscard]] std::optional<FrameSyncResult> synchronize(const std::vector<Sample> &samples) const;
    [[nodiscard]] std::optional<FrameSyncResult> synchronize(std::span<const Sample> samples,
                                                             Scratch &scratch) const;

    // Convenience accessors for derived parameters and reference chirps
    [[nodiscard]] std::size_t samples_per_symbol() const { return sps_; }
    [[nodiscard]] const std::vector<std::complex<double>> &upchirp() const { return upchirp_; }
    [[nodiscard]] const std::vector<std::complex<double>> &downchirp() const { return downchirp_; }

private:
    // Configuration
    int sf_ = 7;
    int bandwidth_hz_ = 125000;
    int sample_rate_hz_ = 500000;
    // Oversampling factor = Fs/BW (integral)
    std::size_t os_factor_ = 4;
    // Samples per LoRa symbol at current SF/BW/FS
    std::size_t sps_ = 0; // samples per symbol

    // Precomputed reference chirps for dechirping and matched filtering
    std::vector<std::complex<double>> upchirp_;
    std::vector<std::complex<double>> downchirp_;
};

// Streaming wrapper around `FrameSynchronizer` that maintains a rolling buffer
// and can process arbitrarily sized sample chunks. It exposes detection state
// and enables consumers to align subsequent decoders without re-copying data.
class StreamingFrameSynchronizer {
public:
    using Sample = FrameSynchronizer::Sample;

    // Initialize streaming synchronizer with LoRa parameters.
    StreamingFrameSynchronizer(int sf, int bandwidth_hz, int sample_rate_hz);

    // Reset all internal state and clear buffers.
    void reset();

    // Seed the internal buffer with an initial set of samples and establish
    // a global offset that identifies the absolute index of buffer()[0].
    void prime(std::span<const Sample> samples, std::size_t global_offset = 0);

    // Ingest a new chunk of samples, update rolling detection state, and return
    // a new detection if a preamble consistent with a frame start is found.
    // The returned offsets are with respect to the internal buffer/global index.
    [[nodiscard]] std::optional<FrameSyncResult> update(std::span<const Sample> chunk);

    // Observability helpers
    [[nodiscard]] std::size_t total_samples() const { return total_samples_ingested_; }
    [[nodiscard]] std::size_t buffer_global_offset() const { return buffer_global_offset_; }
    [[nodiscard]] const std::vector<Sample> &buffer() const { return buffer_; }
    [[nodiscard]] std::optional<FrameSyncResult> detection() const { return detection_; }

    // Inform the synchronizer that the consumer has processed/consumed the
    // first `samples` from the buffer. This advances the global offset and
    // trims internal storage to keep memory bounded.
    void consume(std::size_t samples);

private:
    // Underlying batch synchronizer used for local detections
    FrameSynchronizer base_;

    // Scratch storage reused across update() calls to avoid repeated allocations in embedded builds.
    FrameSynchronizer::Scratch scratch_;

    // Rolling buffer of recent samples; indices relate to buffer_global_offset_
    std::vector<Sample> buffer_;

    // Global (absolute) sample index corresponding to buffer_[0]
    std::size_t buffer_global_offset_ = 0;

    // Monotonic counter of all samples seen across updates/primes
    std::size_t total_samples_ingested_ = 0;

    // Whether a valid detection is currently tracked/locked
    bool locked_ = false;

    // Latest detection result (if any) localized to the current buffer
    std::optional<FrameSyncResult> detection_;

    // Cached samples per symbol for guard calculations and trimming
    std::size_t sps_ = 0;

    // Append new samples to the rolling buffer and update accounting
    void append_samples(std::span<const Sample> samples);

    // How many samples to retain as a guard before the detection point to
    // allow decoders to look-back safely (e.g., for fine timing)
    [[nodiscard]] std::size_t guard_keep_samples() const;

    // Drop leading samples that are no longer needed while preserving enough
    // context around any active detection; updates buffer_global_offset_
    void trim_buffer();
};

} // namespace lora
