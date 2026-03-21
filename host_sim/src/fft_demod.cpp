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

    // Single-tap decimation.
    //
    // At high oversampling (os > 4, e.g. OTA captures at 2 MHz), we
    // MUST use base=0.  The LoRa chirp wraps at n_fold = sps - V*os,
    // which always falls on a chip boundary (multiple of os).  Picking
    // sample 0 of each chip group guarantees every tap sits inside one
    // chirp segment, so the N-point FFT peak lands on the correct bin
    // for any symbol value and any CFO.  Other bases (including the
    // "middle" base) break because the two chirp segments contribute
    // with a phase mismatch that shifts the peak for ~28% of symbols.
    //
    // At low oversampling (os ≤ 4) we keep the legacy base to stay
    // bit-exact with the reference demodulator and existing stage files.
    {
        int base = 0;
        if (oversample_factor_ <= 4) {
            base = oversample_factor_ / 2;
            if (oversample_factor_ > 1 && (oversample_factor_ % 2) == 0) {
                base = std::max(0, base - 1);
            }
        }

        for (int bin = 0; bin < n_bins_; ++bin) {
            const int sample_idx = bin * oversample_factor_ + base;

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

    // Log-domain (Gaussian) parabolic interpolation.
    // More accurate than power-domain for sinc/Dirichlet-shaped peaks,
    // especially when the true peak is far from the bin centre.
    float delta = 0.0f;
    if (mag_prev > 1e-30f && best_mag > 1e-30f && mag_next > 1e-30f) {
        const float lp = std::log(mag_prev);
        const float lb = std::log(best_mag);
        const float ln_next = std::log(mag_next);
        const float denom = lp - 2.0f * lb + ln_next;
        if (std::abs(denom) > 1e-6f) {
            delta = 0.5f * (lp - ln_next) / denom;
            if (delta > 0.5f) {
                delta = 0.5f;
            } else if (delta < -0.5f) {
                delta = -0.5f;
            }
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

    // --- SFO estimation from inter-symbol phase-difference drift ---
    //
    // CFO_frac causes a constant phase rotation Δφ between successive
    // preamble symbols at the peak bin.  SFO causes Δφ to drift linearly.
    //
    // We compute Δφ[k] = arg( fft[k+1] · conj(fft[k]) ) for each pair
    // of adjacent preamble symbols, then fit a linear slope.  The slope
    // in rad/symbol² maps to SFO in bins/symbol via:
    //    sfo_slope = phase_slope / (2π)
    //
    // The alignment may include 1-2 non-preamble symbols at the start.
    // We validate each symbol by checking its FFT peak is at global_bin
    // (±1 bin), and only use phase differences between verified preamble
    // symbol pairs.

    estimate.sfo_slope = 0.0f;

    if (symbol_count >= 6) {
        const int n_diffs = symbol_count - 1;
        const int half = n_bins_ / 2;

        // Mark which symbols are genuine preamble (peak at global_bin ±1)
        std::vector<bool> is_preamble(symbol_count, false);
        for (int s = 0; s < symbol_count; ++s) {
            int sym_best = 0;
            float sym_best_mag = -1.0f;
            for (int bin = 0; bin < n_bins_; ++bin) {
                const auto& v = fft_vals[s][bin];
                const float mag = v.real() * v.real() + v.imag() * v.imag();
                if (mag > sym_best_mag) {
                    sym_best_mag = mag;
                    sym_best = bin;
                }
            }
            int d = std::abs(sym_best - global_bin);
            d = std::min(d, n_bins_ - d);
            is_preamble[s] = (d <= 1);
        }

        // Compute phase differences only between consecutive verified pairs
        std::vector<int>    inlier_indices;
        std::vector<double> inlier_phases;
        for (int k = 0; k < n_diffs; ++k) {
            if (!is_preamble[k] || !is_preamble[k + 1]) continue;
            const std::complex<double> a = fft_vals[k][global_bin];
            const std::complex<double> b = fft_vals[k + 1][global_bin];
            inlier_indices.push_back(k);
            inlier_phases.push_back(std::arg(b * std::conj(a)));
        }

        const int n_inliers = static_cast<int>(inlier_indices.size());
        if (n_inliers >= 4) {
            const double n = static_cast<double>(n_inliers);
            double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_xx = 0.0;
            for (int i = 0; i < n_inliers; ++i) {
                const double x = static_cast<double>(inlier_indices[i]);
                const double y = inlier_phases[i];
                sum_x += x;
                sum_y += y;
                sum_xy += x * y;
                sum_xx += x * x;
            }
            const double denom = n * sum_xx - sum_x * sum_x;
            if (std::abs(denom) > 1e-12) {
                const double slope = (n * sum_xy - sum_x * sum_y) / denom;
                const double intercept = (sum_y - slope * sum_x) / n;

                double ss_res = 0.0;
                for (int i = 0; i < n_inliers; ++i) {
                    const double predicted = intercept + slope * static_cast<double>(inlier_indices[i]);
                    const double residual = inlier_phases[i] - predicted;
                    ss_res += residual * residual;
                }

                const double s2 = ss_res / (n - 2.0);
                const double x_bar = sum_x / n;
                double ss_xx = 0.0;
                for (int i = 0; i < n_inliers; ++i) {
                    const double dx = static_cast<double>(inlier_indices[i]) - x_bar;
                    ss_xx += dx * dx;
                }

                const double se_slope = (ss_xx > 1e-12) ? std::sqrt(s2 / ss_xx) : 1e30;
                const double t_stat = std::abs(slope) / se_slope;
                const double sfo = slope / (2.0 * M_PI);

                if (std::getenv("HOST_SIM_DEBUG_SFO")) {
                    std::cerr << "[SFO_debug] n_diffs=" << n_diffs
                              << " n_inliers=" << n_inliers
                              << " slope_rad=" << slope
                              << " sfo_bins=" << sfo
                              << " se_slope=" << se_slope
                              << " t_stat=" << t_stat
                              << "\n";
                    for (int i = 0; i < n_inliers; ++i) {
                        std::cerr << "[SFO_debug]   phase[" << inlier_indices[i]
                                  << "]=" << inlier_phases[i] << "\n";
                    }
                }

                // Accept SFO only if:
                // 1. Statistically significant (t > 3.0)
                // 2. Physically plausible (< 0.1 bins/symbol)
                // 3. Large enough to matter (> 0.001 bins/symbol)
                if (t_stat > 3.0 && std::abs(sfo) < 0.1 && std::abs(sfo) > 0.001) {
                    estimate.sfo_slope = static_cast<float>(sfo);
                }
            }
        }
    }

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
