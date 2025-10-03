#pragma once

#include <complex>
#include <cstddef>
#include <optional>
#include <vector>

namespace lora {

struct PreambleDetection {
    std::size_t offset = 0;
    float metric = 0.0f;
};

class PreambleDetector {
public:
    using Sample = std::complex<float>;

    PreambleDetector(int sf, int bandwidth_hz, int sample_rate_hz);

    [[nodiscard]] std::optional<PreambleDetection> detect(const std::vector<Sample> &samples) const;

    [[nodiscard]] std::size_t samples_per_symbol() const { return sps_; }

private:
    int sf_ = 7;
    int bandwidth_hz_ = 125000;
    int sample_rate_hz_ = 500000;
    std::size_t sps_ = 0;
    std::vector<std::complex<double>> reference_upchirp_;
};

} // namespace lora
