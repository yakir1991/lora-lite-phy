#pragma once

#include <complex>
#include <cstddef>
#include <optional>
#include <vector>

namespace lora {

struct FrameSyncResult {
    // coarse symbol start (samples) at the detected preamble beginning.
    std::ptrdiff_t preamble_offset = 0;
    // fine-aligned start index (samples) to be used by header/payload decoders.
    // This includes any fractional correction derived from fine search.
    std::ptrdiff_t p_ofs_est = 0;
    // carrier frequency offset estimate in Hz, to be used for CFO rotation.
    double cfo_hz = 0.0;
};

class FrameSynchronizer {
public:
    using Sample = std::complex<float>;

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

private:
    int sf_ = 7;
    int bandwidth_hz_ = 125000;
    int sample_rate_hz_ = 500000;
    std::size_t os_factor_ = 4;
    std::size_t sps_ = 0; // samples per symbol

    std::vector<std::complex<double>> upchirp_;
    std::vector<std::complex<double>> downchirp_;
};

} // namespace lora
