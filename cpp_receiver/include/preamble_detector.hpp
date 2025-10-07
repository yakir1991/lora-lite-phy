#pragma once

#include <complex>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace lora {

struct PreambleDetection {
    // Estimated start offset (in samples) of the LoRa preamble.
    std::size_t offset = 0;
    // Detection quality metric (unitless). Higher is better.
    float metric = 0.0f;
};

class PreambleDetector {
public:
    using Sample = std::complex<float>;

    struct Scratch {
        std::vector<std::complex<double>> window;

        // Ensure the matched-filter window has the requested capacity and return it.
        [[nodiscard]] std::vector<std::complex<double>> &ensure_window(std::size_t required) {
            window.resize(required);
            return window;
        }
    };

    // Construct a LoRa preamble detector.
    // Parameters:
    //  - sf: Spreading factor (5..12)
    //  - bandwidth_hz: LoRa bandwidth in Hz
    //  - sample_rate_hz: complex sample rate in Hz; must be an integer multiple of BW
    PreambleDetector(int sf, int bandwidth_hz, int sample_rate_hz);

    struct MutableSampleSpan {
        Sample *data = nullptr;
        std::size_t capacity = 0;
        constexpr MutableSampleSpan() = default;
        constexpr MutableSampleSpan(Sample *ptr, std::size_t cap) : data(ptr), capacity(cap) {}
    };

    // Run matched-filter style detection over the input samples to locate the
    // preamble start. Returns std::nullopt if no convincing peak is found.
    [[nodiscard]] std::optional<PreambleDetection> detect(const std::vector<Sample> &samples) const;
    [[nodiscard]] std::optional<PreambleDetection> detect(std::span<const Sample> samples,
                                                          Scratch &scratch) const;

    // Convenience: number of samples per symbol for the configured SF/BW/Fs.
    [[nodiscard]] std::size_t samples_per_symbol() const { return sps_; }

private:
    int sf_ = 7;
    int bandwidth_hz_ = 125000;
    int sample_rate_hz_ = 500000;
    std::size_t sps_ = 0;
    std::vector<std::complex<double>> reference_upchirp_;
};

} // namespace lora
