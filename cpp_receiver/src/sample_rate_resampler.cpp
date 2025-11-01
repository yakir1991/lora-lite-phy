#include "sample_rate_resampler.hpp"

#ifdef LORA_ENABLE_LIQUID_DSP
#include "liquid.h"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr double kDefaultAttenuationDb = 60.0;
constexpr double kRatioDisableThreshold = 1e-6;

} // namespace

namespace lora {

using Sample = SampleRateResampler::Sample;

#ifdef LORA_ENABLE_LIQUID_DSP
struct SampleRateResampler::LiquidHandle {
    explicit LiquidHandle(float rate, float attenuation)
        : resamp(msresamp_crcf_create(rate, attenuation)) {}

    ~LiquidHandle() {
        if (resamp) {
            msresamp_crcf_destroy(resamp);
            resamp = nullptr;
        }
    }

    LiquidHandle(const LiquidHandle &) = delete;
    LiquidHandle &operator=(const LiquidHandle &) = delete;

    LiquidHandle(LiquidHandle &&other) noexcept
        : resamp(std::exchange(other.resamp, nullptr)) {}

    LiquidHandle &operator=(LiquidHandle &&other) noexcept {
        if (this != &other) {
            if (resamp) {
                msresamp_crcf_destroy(resamp);
            }
            resamp = std::exchange(other.resamp, nullptr);
        }
        return *this;
    }

    msresamp_crcf resamp = nullptr;
};
#endif

SampleRateResampler::SampleRateResampler() = default;

SampleRateResampler::~SampleRateResampler() {
    destroy();
}

SampleRateResampler::SampleRateResampler(SampleRateResampler &&other) noexcept {
    *this = std::move(other);
}

SampleRateResampler &SampleRateResampler::operator=(SampleRateResampler &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    destroy();
    sample_rate_ratio_ = other.sample_rate_ratio_;
    resample_factor_ = other.resample_factor_;
    attenuation_db_ = other.attenuation_db_;
    active_ = other.active_;
#ifdef LORA_ENABLE_LIQUID_DSP
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    buffer_ = std::move(other.buffer_);
    cursor_ = other.cursor_;
    other.cursor_ = 0.0;
#endif
    return *this;
}

void SampleRateResampler::destroy() {
#ifdef LORA_ENABLE_LIQUID_DSP
    delete handle_;
    handle_ = nullptr;
#else
    buffer_.clear();
    cursor_ = 0.0;
#endif
}

void SampleRateResampler::configure(double sample_rate_ratio, double attenuation_db) {
    if (sample_rate_ratio <= 0.0 ||
        !std::isfinite(sample_rate_ratio)) {
        sample_rate_ratio = 1.0;
    }

    sample_rate_ratio_ = sample_rate_ratio;
    attenuation_db_ = std::isfinite(attenuation_db) ? attenuation_db : kDefaultAttenuationDb;
    resample_factor_ = 1.0 / sample_rate_ratio_;
    active_ = std::abs(resample_factor_ - 1.0) > kRatioDisableThreshold;

#ifdef LORA_ENABLE_LIQUID_DSP
    delete handle_;
    handle_ = nullptr;
    if (active_) {
        const float rate = static_cast<float>(resample_factor_);
        const float att = static_cast<float>(attenuation_db_);
        handle_ = new LiquidHandle(rate, att);
    }
#else
    buffer_.clear();
    cursor_ = 0.0;
#endif
}

void SampleRateResampler::reset() {
#ifdef LORA_ENABLE_LIQUID_DSP
    if (handle_ && handle_->resamp) {
        msresamp_crcf_reset(handle_->resamp);
    }
#else
    buffer_.clear();
    cursor_ = 0.0;
#endif
}

#ifdef LORA_ENABLE_LIQUID_DSP
void SampleRateResampler::ensure_handle() {
    if (!handle_) {
        const float rate = static_cast<float>(resample_factor_);
        const float att = static_cast<float>(attenuation_db_);
        handle_ = new LiquidHandle(rate, att);
    }
}
#endif

std::size_t SampleRateResampler::process(std::span<const Sample> input, std::vector<Sample> &dest) {
    if (input.empty()) {
        return 0;
    }

    if (!active_) {
        dest.insert(dest.end(), input.begin(), input.end());
        return input.size();
    }

#ifdef LORA_ENABLE_LIQUID_DSP
    ensure_handle();
    if (!handle_ || !handle_->resamp) {
        return 0;
    }
    const std::size_t start = dest.size();
    const double guard = std::ceil(static_cast<double>(input.size()) * resample_factor_) + 8.0;
    dest.resize(start + static_cast<std::size_t>(guard));
    unsigned int produced = 0;
    msresamp_crcf_execute(handle_->resamp,
                          const_cast<liquid_float_complex *>(reinterpret_cast<const liquid_float_complex *>(input.data())),
                          static_cast<unsigned int>(input.size()),
                          reinterpret_cast<liquid_float_complex *>(dest.data() + start),
                          &produced);
    dest.resize(start + produced);
    return produced;
#else
    buffer_.insert(buffer_.end(), input.begin(), input.end());
    const double step = 1.0 / resample_factor_;
    std::size_t produced = 0;
    while (buffer_.size() >= 2 && cursor_ + 1.0 < static_cast<double>(buffer_.size())) {
        const auto idx0 = static_cast<std::size_t>(cursor_);
        const double frac = cursor_ - static_cast<double>(idx0);
        const Sample &s0 = buffer_[idx0];
        const Sample &s1 = buffer_[idx0 + 1];
        const Sample interp = static_cast<float>(1.0 - frac) * s0 + static_cast<float>(frac) * s1;
        dest.push_back(interp);
        cursor_ += step;
        produced++;
    }

    const std::size_t consumed = static_cast<std::size_t>(std::min(cursor_, static_cast<double>(buffer_.size())));
    if (consumed > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
        cursor_ -= static_cast<double>(consumed);
    }
    return produced;
#endif
}

std::size_t SampleRateResampler::flush(std::vector<Sample> &dest) {
    if (!active_) {
        return 0;
    }

#ifdef LORA_ENABLE_LIQUID_DSP
    ensure_handle();
    if (!handle_ || !handle_->resamp) {
        return 0;
    }
    const float delay = msresamp_crcf_get_delay(handle_->resamp);
    const std::size_t pad = static_cast<std::size_t>(std::ceil(delay)) + 4;
    if (pad == 0) {
        return 0;
    }
    std::vector<Sample> zeros(pad, Sample{0.0f, 0.0f});
    const std::size_t start = dest.size();
    const double guard = std::ceil(static_cast<double>(pad) * resample_factor_) + 4.0;
    dest.resize(start + static_cast<std::size_t>(guard));
    unsigned int produced = 0;
    msresamp_crcf_execute(handle_->resamp,
                          const_cast<liquid_float_complex *>(reinterpret_cast<const liquid_float_complex *>(zeros.data())),
                          static_cast<unsigned int>(zeros.size()),
                          reinterpret_cast<liquid_float_complex *>(dest.data() + start),
                          &produced);
    dest.resize(start + produced);
    return produced;
#else
    std::size_t produced = 0;
    const double step = 1.0 / resample_factor_;
    while (buffer_.size() >= 2 && cursor_ + 1.0 < static_cast<double>(buffer_.size())) {
        const auto idx0 = static_cast<std::size_t>(cursor_);
        const double frac = cursor_ - static_cast<double>(idx0);
        const Sample &s0 = buffer_[idx0];
        const Sample &s1 = buffer_[idx0 + 1];
        const Sample interp = static_cast<float>(1.0 - frac) * s0 + static_cast<float>(frac) * s1;
        dest.push_back(interp);
        cursor_ += step;
        produced++;
    }
    if (!buffer_.empty()) {
        dest.push_back(buffer_.back());
        produced++;
    }
    buffer_.clear();
    cursor_ = 0.0;
    return produced;
#endif
}

std::vector<Sample> SampleRateResampler::resample(std::span<const Sample> input) {
    std::vector<Sample> output;
    if (!active_) {
        output.assign(input.begin(), input.end());
        return output;
    }

    output.reserve(static_cast<std::size_t>(std::ceil(static_cast<double>(input.size()) * resample_factor_)) + 8);
    reset();
    process(input, output);
    flush(output);
    return output;
}

} // namespace lora
