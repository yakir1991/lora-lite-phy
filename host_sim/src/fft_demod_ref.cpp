#include "host_sim/fft_demod_ref.hpp"

#include <cmath>
#include <stdexcept>

namespace host_sim
{

namespace
{
std::vector<std::complex<float>> build_downchirp(int sf, int oversample_factor)
{
    const int n_bins = 1 << sf;
    const int samples_per_symbol = n_bins * oversample_factor;
    const float N = static_cast<float>(n_bins);
    const float os = static_cast<float>(oversample_factor);

    std::vector<std::complex<float>> downchirp(samples_per_symbol);
    for (int n = 0; n < samples_per_symbol; ++n) {
        const float chip = static_cast<float>(n) / os;
        const float quadratic = chip * chip / (2.0f * N);
        const float linear = -0.5f * chip;
        const float phase = quadratic + linear;
        downchirp[n] = std::complex<float>(std::cos(-2.0f * static_cast<float>(M_PI) * phase),
                                           std::sin(-2.0f * static_cast<float>(M_PI) * phase));
    }
    return downchirp;
}
} // namespace

FftDemodReference::FftDemodReference(int sf, int sample_rate, int bandwidth)
    : sf_(sf),
      n_bins_(1 << sf),
      sample_rate_(sample_rate),
      bandwidth_(bandwidth)
{
    oversample_factor_ = bandwidth_ > 0 ? sample_rate_ / bandwidth_ : 1;
    if (oversample_factor_ <= 0) {
        oversample_factor_ = 1;
    }
    samples_per_symbol_ = n_bins_ * oversample_factor_;
    downchirp_ = build_downchirp(sf_, oversample_factor_);
    initialize_fft();
}

FftDemodReference::~FftDemodReference()
{
    if (kiss_cfg_ != nullptr) {
        free(kiss_cfg_);
        kiss_cfg_ = nullptr;
    }
}

void FftDemodReference::initialize_fft()
{
    kiss_cfg_ = kiss_fft_alloc(n_bins_, 0, nullptr, nullptr);
    if (!kiss_cfg_) {
        throw std::runtime_error("Failed to allocate KISS FFT configuration for reference demod");
    }
    fft_input_.resize(n_bins_);
    fft_output_.resize(n_bins_);
}

uint16_t FftDemodReference::demodulate(const std::complex<float>* symbol_samples) const
{
    const bool apply_sfo = std::abs(sfo_slope_) > 0.0f;
    const float sfo_factor = apply_sfo
        ? -2.0f * static_cast<float>(M_PI) * sfo_slope_ /
              static_cast<float>(samples_per_symbol_)
        : 0.0f;

    int base = oversample_factor_ / 2;
    if (oversample_factor_ > 1 && (oversample_factor_ % 2) == 0) {
        base = std::max(0, base - 1);
    }

    for (int bin = 0; bin < n_bins_; ++bin) {
        int sample_idx = bin * oversample_factor_ + base;
        if (sample_idx < 0) {
            sample_idx = 0;
        } else if (sample_idx >= samples_per_symbol_) {
            sample_idx = samples_per_symbol_ - 1;
        }
        const float chip = static_cast<float>(sample_idx) /
                           static_cast<float>(oversample_factor_);
        const float phase = -2.0f * static_cast<float>(M_PI) * fractional_offset_ *
                            chip / static_cast<float>(n_bins_);
        std::complex<float> rot(std::cos(phase), std::sin(phase));
        if (apply_sfo) {
            const float sfo_phase = sfo_factor * static_cast<float>(sample_idx);
            rot *= std::complex<float>(std::cos(sfo_phase), std::sin(sfo_phase));
        }
        const std::complex<float> value = symbol_samples[sample_idx] * rot * downchirp_[sample_idx];
        fft_input_[bin].r = value.real();
        fft_input_[bin].i = value.imag();
    }

    kiss_fft(kiss_cfg_, fft_input_.data(), fft_output_.data());

    int best_bin = 0;
    float best_mag = -1.0f;
    for (int bin = 0; bin < n_bins_; ++bin) {
        const float re = fft_output_[bin].r;
        const float im = fft_output_[bin].i;
        const float magnitude_sq = re * re + im * im;
        if (magnitude_sq > best_mag) {
            best_mag = magnitude_sq;
            best_bin = bin;
        }
    }

    int corrected_bin = best_bin - cfo_int_;
    corrected_bin %= n_bins_;
    if (corrected_bin < 0) {
        corrected_bin += n_bins_;
    }

    return static_cast<uint16_t>(corrected_bin);
}

void FftDemodReference::set_frequency_offsets(float fractional, float sfo_slope, int cfo_int)
{
    fractional_offset_ = fractional;
    sfo_slope_ = sfo_slope;
    cfo_int_ = cfo_int;
}

} // namespace host_sim
