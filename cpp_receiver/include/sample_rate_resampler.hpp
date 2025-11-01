#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace lora {

// Wrapper around Liquid-DSP's msresamp (when available) providing a reusable
// complex<float> fractional resampler. The interface is chunk-friendly so both
// batch and streaming receivers can resample without duplicating logic.
class SampleRateResampler {
public:
    using Sample = std::complex<float>;

    SampleRateResampler();
    ~SampleRateResampler();

    SampleRateResampler(SampleRateResampler &&other) noexcept;
    SampleRateResampler &operator=(SampleRateResampler &&other) noexcept;

    SampleRateResampler(const SampleRateResampler &) = delete;
    SampleRateResampler &operator=(const SampleRateResampler &) = delete;

    // Configure the converter. `sample_rate_ratio` is (actual/nominal); the
    // resampler internally converts to target/input = 1/ratio so the output
    // stream matches the nominal sample spacing. Ratios sufficiently close to
    // 1.0 disable resampling automatically.
    void configure(double sample_rate_ratio, double attenuation_db = 60.0);

    [[nodiscard]] double sample_rate_ratio() const { return sample_rate_ratio_; }
    [[nodiscard]] double resample_factor() const { return resample_factor_; }
    [[nodiscard]] bool active() const { return active_; }

    // Reset internal state while preserving the last configuration.
    void reset();

    // Process a chunk of samples, appending output to `dest`. Returns the number
    // of new output samples appended.
    std::size_t process(std::span<const Sample> input, std::vector<Sample> &dest);

    // Flush outstanding filter delay. Should be called once after the final
    // chunk to push residual samples into `dest`.
    std::size_t flush(std::vector<Sample> &dest);

    // Convenience helper for batch use: resample entire vector in one call.
    [[nodiscard]] std::vector<Sample> resample(std::span<const Sample> input);

private:
    void destroy();
#ifdef LORA_ENABLE_LIQUID_DSP
    void ensure_handle();
#endif

    double sample_rate_ratio_ = 1.0;
    double resample_factor_ = 1.0;
    double attenuation_db_ = 60.0;
    bool active_ = false;

#ifdef LORA_ENABLE_LIQUID_DSP
    struct LiquidHandle;
    LiquidHandle *handle_ = nullptr;
#else
    std::vector<Sample> buffer_;
    double cursor_ = 0.0;
#endif
};

} // namespace lora

