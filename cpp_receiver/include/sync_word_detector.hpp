#pragma once

#include <complex>
#include <cstddef>
#include <optional>
#include <vector>

namespace lora {

struct SyncWordDetection {
    std::size_t preamble_offset = 0;
    std::vector<int> symbol_bins;
    std::vector<double> magnitudes;
    bool preamble_ok = false;
    bool sync_ok = false;
};

class SyncWordDetector {
public:
    using Sample = std::complex<float>;

    SyncWordDetector(int sf, int bandwidth_hz, int sample_rate_hz, unsigned sync_word);

    [[nodiscard]] std::optional<SyncWordDetection> analyze(const std::vector<Sample> &samples,
                                                           std::size_t preamble_offset) const;

    [[nodiscard]] std::size_t samples_per_symbol() const { return sps_; }

private:
    struct FFTScratch {
        std::vector<std::complex<double>> input;
        std::vector<std::complex<double>> spectrum;
    };

    [[nodiscard]] std::size_t demod_symbol(const std::vector<Sample> &samples,
                                           std::size_t sym_index,
                                           std::size_t preamble_offset,
                                           FFTScratch &scratch,
                                           double &magnitude) const;

    int sf_ = 7;
    int bandwidth_hz_ = 125000;
    int sample_rate_hz_ = 500000;
    unsigned sync_word_ = 0x12;
    std::size_t sps_ = 0;
    std::vector<std::complex<double>> downchirp_;
};

} // namespace lora
