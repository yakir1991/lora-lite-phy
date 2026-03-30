/// Native Q15 LoRa FFT demodulator.
///
/// The downchirp multiply and FFT run entirely in Q15 fixed-point
/// arithmetic via KissFFT compiled with FIXED_POINT=16.  Only the
/// final parabolic interpolation and CFO-tracking EMA use float.

#include "host_sim/fft_demod_q15.hpp"
#include "host_sim/q15.hpp"

extern "C" {
#include "kiss_fft_q15.h"
}

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace host_sim
{

FftDemodulatorQ15::FftDemodulatorQ15(int sf, int sample_rate, int bandwidth)
    : sf_(sf),
      n_bins_(1 << sf),
      sample_rate_(sample_rate),
      bandwidth_(bandwidth),
      oversample_factor_(std::max(1, sample_rate / bandwidth)),
      samples_per_symbol_((1 << sf) * oversample_factor_),
      chirps_q15_(build_chirps_q15(sf, oversample_factor_))
{
    kiss_cfg_ = kiss_fft_q15_alloc(n_bins_, 0, nullptr, nullptr);
    fft_in_.resize(n_bins_);
    fft_out_.resize(n_bins_);
}

FftDemodulatorQ15::~FftDemodulatorQ15()
{
    if (kiss_cfg_) {
        kiss_fft_q15_free(kiss_cfg_);
        kiss_cfg_ = nullptr;
    }
}

void FftDemodulatorQ15::set_input_scale(float scale)
{
    input_scale_ = (scale > 0.0f) ? scale : 1.0f;
}

void FftDemodulatorQ15::set_frequency_offsets(float cfo_frac, int cfo_int,
                                              float sfo_slope)
{
    cfo_frac_ = cfo_frac;
    cfo_int_  = cfo_int;
    sfo_slope_ = sfo_slope;
}

void FftDemodulatorQ15::set_cfo_tracking(float alpha, int delay_symbols)
{
    cfo_track_alpha_ = alpha;
    cfo_track_delay_ = delay_symbols;
}

uint16_t FftDemodulatorQ15::demodulate(const Q15Complex* symbol_samples)
{
    const float fractional_offset =
        cfo_frac_ + sfo_slope_ * static_cast<float>(symbol_counter_);
    const bool apply_sfo = std::abs(sfo_slope_) > 0.0f;
    const float sfo_factor = apply_sfo
        ? -2.0f * static_cast<float>(M_PI) * sfo_slope_ *
              static_cast<float>(symbol_counter_) /
              static_cast<float>(samples_per_symbol_)
        : 0.0f;

    // Base sample selection (same logic as float demod).
    int base = 0;
    if (oversample_factor_ <= 4) {
        base = oversample_factor_ / 2;
        if (oversample_factor_ > 1 && (oversample_factor_ % 2) == 0) {
            base = std::max(0, base - 1);
        }
    }

    // Single-tap decimation: downchirp multiply with phase rotation, all Q15.
    for (int bin = 0; bin < n_bins_; ++bin) {
        const int sample_idx = bin * oversample_factor_ + base;

        // Per-sample phase rotation for CFO/SFO.
        const float chip = static_cast<float>(sample_idx) /
                           static_cast<float>(oversample_factor_);
        float phase = -2.0f * static_cast<float>(M_PI) * fractional_offset *
                      chip / static_cast<float>(n_bins_);
        if (apply_sfo) {
            phase += sfo_factor * static_cast<float>(sample_idx);
        }
        const Q15Complex rot = float_to_q15_complex(std::cos(phase),
                                                    std::sin(phase));

        // sample × rotation × downchirp — two Q15 complex multiplies.
        const Q15Complex rotated = q15_mul(symbol_samples[sample_idx], rot);
        const Q15Complex dechirped = q15_mul(rotated,
                                             chirps_q15_.downchirp[sample_idx]);

        fft_in_[bin].r = dechirped.real;
        fft_in_[bin].i = dechirped.imag;
    }

    // Q15 FFT.
    kiss_fft_q15(kiss_cfg_,
                 reinterpret_cast<const kiss_fft_q15_cpx*>(fft_in_.data()),
                 reinterpret_cast<kiss_fft_q15_cpx*>(fft_out_.data()));

    // Peak detection in Q31 (int32 magnitude-squared avoids overflow from
    // int16 * int16 and gives ample dynamic range for comparison).
    int best_bin = 0;
    int second_bin = 0;
    int64_t best_mag = -1;
    int64_t second_mag = -1;
    for (int bin = 0; bin < n_bins_; ++bin) {
        const int32_t re = fft_out_[bin].r;
        const int32_t im = fft_out_[bin].i;
        const int64_t mag_sq = static_cast<int64_t>(re) * re +
                               static_cast<int64_t>(im) * im;
        if (mag_sq > best_mag) {
            second_mag = best_mag;
            second_bin = best_bin;
            best_mag = mag_sq;
            best_bin = bin;
        } else if (mag_sq > second_mag) {
            second_mag = mag_sq;
            second_bin = bin;
        }
    }

    // Resolve single-bin ambiguity (adjacent bins with near-equal power).
    if (second_mag >= 0) {
        const int64_t diff = best_mag - second_mag;
        const int64_t threshold = best_mag / 1000000 + 1;
        if (diff < threshold && std::abs(second_bin - best_bin) == 1) {
            if (second_bin > best_bin) {
                best_bin = second_bin;
            }
        }
    }

    // Convert neighbour magnitudes to float for log-domain parabolic
    // interpolation (a few floats are acceptable for the final step).
    const int prev_bin = (best_bin - 1 + n_bins_) % n_bins_;
    const int next_bin = (best_bin + 1) % n_bins_;

    auto mag_sq_f = [&](int b) -> float {
        const float re = static_cast<float>(fft_out_[b].r);
        const float im = static_cast<float>(fft_out_[b].i);
        return re * re + im * im;
    };
    const float fp = mag_sq_f(prev_bin);
    const float fb = mag_sq_f(best_bin);
    const float fn = mag_sq_f(next_bin);

    float delta = 0.0f;
    if (fp > 1e-30f && fb > 1e-30f && fn > 1e-30f) {
        const float lp = std::log(fp);
        const float lb = std::log(fb);
        const float ln_next = std::log(fn);
        const float denom = lp - 2.0f * lb + ln_next;
        if (std::abs(denom) > 1e-6f) {
            delta = 0.5f * (lp - ln_next) / denom;
            delta = std::clamp(delta, -0.5f, 0.5f);
        }
    }

    float corrected_position = static_cast<float>(best_bin) + delta -
                               static_cast<float>(cfo_int_);
    int corrected_bin = static_cast<int>(std::lround(corrected_position));
    corrected_bin %= n_bins_;
    if (corrected_bin < 0) {
        corrected_bin += n_bins_;
    }

    // Per-symbol closed-loop CFO tracking.
    if (cfo_track_alpha_ > 0.0f &&
        static_cast<int>(symbol_counter_) >= cfo_track_delay_) {
        const float residual = corrected_position -
                               static_cast<float>(corrected_bin);
        cfo_frac_ += cfo_track_alpha_ * residual;
        if (cfo_frac_ >= 0.5f)  cfo_frac_ -= 1.0f;
        if (cfo_frac_ < -0.5f)  cfo_frac_ += 1.0f;
    }

    ++symbol_counter_;
    return static_cast<uint16_t>(corrected_bin);
}

} // namespace host_sim
