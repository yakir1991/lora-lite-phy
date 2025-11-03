#include "host_sim/fft_demod.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <iostream>
#include <stdexcept>

namespace host_sim
{

FftDemodulator::FftDemodulator(int sf, int sample_rate, int bandwidth)
    : sf_(sf),
      n_bins_(1 << sf),
      sample_rate_(sample_rate),
      bandwidth_(bandwidth)
{
    oversample_factor_ = sample_rate_ / bandwidth_;
    if (oversample_factor_ <= 0) {
        oversample_factor_ = 1;
    }
    samples_per_symbol_ = n_bins_ * oversample_factor_;
    chirps_ = build_chirps(sf_, oversample_factor_);
    initialize_fft();
}

FftDemodulator::~FftDemodulator()
{
    if (kiss_cfg_ != nullptr) {
        free(kiss_cfg_);
        kiss_cfg_ = nullptr;
    }
}

void FftDemodulator::initialize_fft()
{
    if (kiss_cfg_ != nullptr) {
        free(kiss_cfg_);
        kiss_cfg_ = nullptr;
    }
    kiss_cfg_ = kiss_fft_alloc(n_bins_, 0, nullptr, nullptr);
    if (!kiss_cfg_) {
        throw std::runtime_error("Failed to allocate KISS FFT configuration");
    }
    fft_in_.resize(n_bins_);
    fft_out_.resize(n_bins_);
    symbol_counter_ = 0;
}

void FftDemodulator::compute_fft(const std::complex<float>* symbol_samples,
                                 kiss_fft_cpx* output) const
{
    for (int bin = 0; bin < n_bins_; ++bin) {
        std::complex<float> acc{0.0f, 0.0f};
        for (int m = 0; m < oversample_factor_; ++m) {
            const int idx = bin * oversample_factor_ + m;
            acc += symbol_samples[idx] * chirps_.downchirp[idx];
        }
        fft_in_[bin].r = acc.real();
        fft_in_[bin].i = acc.imag();
    }

    kiss_fft(kiss_cfg_, fft_in_.data(), fft_out_.data());
    if (output != nullptr) {
        std::copy(fft_out_.begin(), fft_out_.end(), output);
    }
}

uint16_t FftDemodulator::demodulate(const std::complex<float>* symbol_samples) const
{
    const bool debug_fft = (std::getenv("HOST_SIM_DEBUG_FFT_DETAIL") != nullptr);
    const float fractional_offset =
        cfo_frac_ + sfo_slope_ * static_cast<float>(symbol_counter_);
    const bool apply_sfo = std::abs(sfo_slope_) > 0.0f;
    const float sfo_factor = apply_sfo
        ? -2.0f * static_cast<float>(M_PI) * sfo_slope_ * static_cast<float>(symbol_counter_) /
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
        const float phase = -2.0f * static_cast<float>(M_PI) * fractional_offset *
                            chip / static_cast<float>(n_bins_);
        std::complex<float> rot(std::cos(phase), std::sin(phase));
        if (apply_sfo) {
            const float sfo_phase = sfo_factor * static_cast<float>(sample_idx);
            rot *= std::complex<float>(std::cos(sfo_phase), std::sin(sfo_phase));
        }

        const std::complex<float> value =
            symbol_samples[sample_idx] * rot * chirps_.downchirp[sample_idx];
        fft_in_[bin].r = value.real();
        fft_in_[bin].i = value.imag();
    }

    kiss_fft(kiss_cfg_, fft_in_.data(), fft_out_.data());

    int best_bin = 0;
    int second_bin = 0;
    float best_mag = -1.0f;
    float second_mag = -1.0f;
    for (int bin = 0; bin < n_bins_; ++bin) {
        const float re = fft_out_[bin].r;
        const float im = fft_out_[bin].i;
        const float magnitude_sq = re * re + im * im;
        if (magnitude_sq > best_mag) {
            second_mag = best_mag;
            second_bin = best_bin;
            best_mag = magnitude_sq;
            best_bin = bin;
        } else if (magnitude_sq > second_mag) {
            second_mag = magnitude_sq;
            second_bin = bin;
        }
    }

    if (second_mag >= 0.0f) {
        const float diff = best_mag - second_mag;
        const float threshold = best_mag * 1e-6f + 1e-6f;
        if (diff < threshold && std::abs(second_bin - best_bin) == 1) {
            if (second_bin > best_bin) {
                best_bin = second_bin;
            }
        }
    }

    const int prev_bin = (best_bin - 1 + n_bins_) % n_bins_;
    const int next_bin = (best_bin + 1) % n_bins_;
    const float mag_prev = fft_out_[prev_bin].r * fft_out_[prev_bin].r +
                           fft_out_[prev_bin].i * fft_out_[prev_bin].i;
    const float mag_next = fft_out_[next_bin].r * fft_out_[next_bin].r +
                           fft_out_[next_bin].i * fft_out_[next_bin].i;
    const float denom = mag_prev - 2.0f * best_mag + mag_next;
    float delta = 0.0f;
    if (std::abs(denom) > 1e-6f) {
        delta = 0.5f * (mag_prev - mag_next) / denom;
        if (delta > 0.5f) {
            delta = 0.5f;
        } else if (delta < -0.5f) {
            delta = -0.5f;
        }
    }

    float corrected_position = static_cast<float>(best_bin) + delta -
                               static_cast<float>(cfo_int_);
    int corrected_bin = static_cast<int>(std::lround(corrected_position));
    corrected_bin %= n_bins_;
    if (corrected_bin < 0) {
        corrected_bin += n_bins_;
    }

    if (debug_fft) {
        std::cout << "[fft-debug] symbol=" << symbol_counter_
                  << " best_bin=" << best_bin
                  << " second_bin=" << second_bin
                  << " best_mag=" << best_mag
                  << " second_mag=" << second_mag
                  << " delta=" << delta
                  << " corrected=" << corrected_bin
                  << '\n';
    }

    ++symbol_counter_;
    return static_cast<uint16_t>(corrected_bin);
}

FftDemodulator::FrequencyEstimate FftDemodulator::estimate_frequency_offsets(
    const std::complex<float>* samples,
    int symbol_count) const
{
    FrequencyEstimate estimate{};
    if (symbol_count <= 0) {
        return estimate;
    }

    std::vector<std::vector<std::complex<float>>> fft_vals(
        symbol_count, std::vector<std::complex<float>>(n_bins_));
    std::vector<float> power_accum(n_bins_, 0.0f);

    std::vector<kiss_fft_cpx> local_fft(n_bins_);
    for (int sym = 0; sym < symbol_count; ++sym) {
        const std::complex<float>* symbol_ptr =
            samples + static_cast<std::size_t>(sym) * samples_per_symbol_;
        compute_fft(symbol_ptr, local_fft.data());

        for (int bin = 0; bin < n_bins_; ++bin) {
            const std::complex<float> value(local_fft[bin].r, local_fft[bin].i);
            fft_vals[sym][bin] = value;
            const float magnitude_sq =
                value.real() * value.real() + value.imag() * value.imag();
            power_accum[bin] += magnitude_sq;
        }
    }

    const int global_bin = static_cast<int>(
        std::distance(power_accum.begin(),
                      std::max_element(power_accum.begin(), power_accum.end())));

    if (symbol_count > 1) {
        std::complex<double> accum{0.0, 0.0};
        for (int sym = 0; sym < symbol_count - 1; ++sym) {
            const std::complex<double> a = fft_vals[sym][global_bin];
            const std::complex<double> b = fft_vals[sym + 1][global_bin];
            accum += a * std::conj(b);
        }
        if (std::abs(accum) > 0.0) {
            estimate.cfo_frac = static_cast<float>(
                -std::arg(accum) / (2.0 * M_PI));
        }
    }

    while (estimate.cfo_frac >= 0.5f) {
        estimate.cfo_frac -= 1.0f;
    }
    while (estimate.cfo_frac < -0.5f) {
        estimate.cfo_frac += 1.0f;
    }

    int signed_bin = global_bin;
    const int half_bins = n_bins_ / 2;
    if (signed_bin > half_bins) {
        signed_bin -= n_bins_;
    }
    estimate.cfo_int = signed_bin;
    estimate.sfo_slope = 0.0f;

    return estimate;
}

void FftDemodulator::set_frequency_offsets(float cfo_frac,
                                           int cfo_int,
                                           float sfo_slope)
{
    while (cfo_frac >= 0.5f) {
        cfo_frac -= 1.0f;
        ++cfo_int;
    }
    while (cfo_frac < -0.5f) {
        cfo_frac += 1.0f;
        --cfo_int;
    }
    cfo_frac_ = cfo_frac;
    cfo_int_ = cfo_int;
    sfo_slope_ = sfo_slope;
    symbol_counter_ = 0;
}

} // namespace host_sim
