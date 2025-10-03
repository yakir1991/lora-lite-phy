#pragma once

#include <complex>
#include <cstddef>
#include <optional>
#include <vector>

namespace lora {

struct FrameSyncResult {
    std::ptrdiff_t preamble_offset = 0;  // coarse symbol start (samples)
    std::ptrdiff_t p_ofs_est = 0;        // fine-aligned start index used by decoder (samples)
    double cfo_hz = 0.0;              // carrier frequency offset estimate in Hz
};

class FrameSynchronizer {
public:
    using Sample = std::complex<float>;

    FrameSynchronizer(int sf, int bandwidth_hz, int sample_rate_hz);

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
