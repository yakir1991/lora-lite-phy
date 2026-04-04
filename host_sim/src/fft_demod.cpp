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
    static const bool debug_fft = (std::getenv("HOST_SIM_DEBUG_FFT_DETAIL") != nullptr);
    const float fractional_offset =
        cfo_frac_ + sfo_slope_ * static_cast<float>(symbol_counter_);
    const bool apply_sfo = std::abs(sfo_slope_) > 0.0f;
    const float sfo_factor = apply_sfo
        ? -2.0f * static_cast<float>(M_PI) * sfo_slope_ * static_cast<float>(symbol_counter_) /
              static_cast<float>(samples_per_symbol_)
        : 0.0f;

    // Single-tap decimation with per-sample CFO/SFO phase correction.
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

    // Store fractional-bin residual for caller-side SFO tracking.
    last_residual_ = corrected_position - static_cast<float>(corrected_bin);

    // Per-symbol closed-loop CFO tracking.
    // The residual (fractional distance from bin centre after all corrections)
    // is a noisy observation of the remaining CFO error.  An EMA filter
    // feeds it back into cfo_frac_ so the correction adapts to slow drift.
    if (cfo_track_alpha_ > 0.0f &&
        static_cast<int>(symbol_counter_) >= cfo_track_delay_) {
        cfo_frac_ += cfo_track_alpha_ * last_residual_;
        // Keep cfo_frac_ in [-0.5, 0.5)
        if (cfo_frac_ >= 0.5f) cfo_frac_ -= 1.0f;
        if (cfo_frac_ < -0.5f) cfo_frac_ += 1.0f;
    }

    ++symbol_counter_;
    return static_cast<uint16_t>(corrected_bin);
}

const std::vector<float>& FftDemodulator::get_fft_magnitudes_sq() const
{
    mag_sq_buf_.resize(n_bins_);
    for (int i = 0; i < n_bins_; ++i) {
        mag_sq_buf_[i] = fft_out_[i].r * fft_out_[i].r + fft_out_[i].i * fft_out_[i].i;
    }
    return mag_sq_buf_;
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

    // --- SFO estimation from fractional-bin drift across preamble ---
    //
    // SFO causes the symbol boundaries to drift, which shifts the
    // dechirped FFT peak by a fraction of a bin per symbol.  We estimate
    // the drift by computing a fractional bin value for each preamble
    // symbol using log-domain Gaussian parabolic interpolation, then
    // fitting a linear slope (OLS) to these values.
    //
    // The slope in bins/symbol directly gives sfo_slope, which is used
    // by the stride-compensated demod loop.

    estimate.sfo_slope = 0.0f;

    if (symbol_count >= 6) {
        // Compute fractional bin for each preamble symbol
        std::vector<int>    inlier_indices;
        std::vector<double> inlier_bins;

        for (int s = 0; s < symbol_count; ++s) {
            // Find peak bin for this symbol
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

            // Verify it's a genuine preamble symbol (peak at global_bin ±1)
            int d = std::abs(sym_best - global_bin);
            d = std::min(d, n_bins_ - d);
            if (d > 1) continue;

            // Parabolic interpolation around the global_bin to get
            // fractional offset (we use global_bin, not sym_best, to
            // keep all measurements on the same baseline).
            const int prev_bin = (global_bin - 1 + n_bins_) % n_bins_;
            const int next_bin = (global_bin + 1) % n_bins_;
            const auto& vp = fft_vals[s][prev_bin];
            const auto& vc = fft_vals[s][global_bin];
            const auto& vn = fft_vals[s][next_bin];
            const float mp = vp.real() * vp.real() + vp.imag() * vp.imag();
            const float mc = vc.real() * vc.real() + vc.imag() * vc.imag();
            const float mn = vn.real() * vn.real() + vn.imag() * vn.imag();

            if (mp > 1e-30f && mc > 1e-30f && mn > 1e-30f) {
                const float lp = std::log(mp);
                const float lc = std::log(mc);
                const float ln_next = std::log(mn);
                const float denom = lp - 2.0f * lc + ln_next;
                if (std::abs(denom) > 1e-6f) {
                    float delta = 0.5f * (lp - ln_next) / denom;
                    delta = std::max(-0.5f, std::min(0.5f, delta));
                    inlier_indices.push_back(s);
                    inlier_bins.push_back(static_cast<double>(delta));
                }
            }
        }

        const int n_inliers = static_cast<int>(inlier_indices.size());
        if (n_inliers >= 4) {
            const double n = static_cast<double>(n_inliers);
            double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_xx = 0.0;
            for (int i = 0; i < n_inliers; ++i) {
                const double x = static_cast<double>(inlier_indices[i]);
                const double y = inlier_bins[i];
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
                    const double residual = inlier_bins[i] - predicted;
                    ss_res += residual * residual;
                }

                const double s2 = ss_res / std::max(1.0, n - 2.0);
                const double x_bar = sum_x / n;
                double ss_xx = 0.0;
                for (int i = 0; i < n_inliers; ++i) {
                    const double dx = static_cast<double>(inlier_indices[i]) - x_bar;
                    ss_xx += dx * dx;
                }

                const double se_slope = (ss_xx > 1e-12) ? std::sqrt(s2 / ss_xx) : 1e30;
                const double t_stat = std::abs(slope) / se_slope;

                // R² (coefficient of determination)
                const double y_bar = sum_y / n;
                double ss_tot = 0.0;
                for (int i = 0; i < n_inliers; ++i) {
                    const double dy = inlier_bins[i] - y_bar;
                    ss_tot += dy * dy;
                }
                const double r_squared = (ss_tot > 1e-12) ? 1.0 - ss_res / ss_tot : 0.0;

                static const bool debug_sfo = (std::getenv("HOST_SIM_DEBUG_SFO") != nullptr);
                if (debug_sfo) {
                    std::cerr << "[SFO_debug] n_inliers=" << n_inliers
                              << " slope_bins=" << slope
                              << " se_slope=" << se_slope
                              << " t_stat=" << t_stat
                              << " r_squared=" << r_squared
                              << "\n";
                    for (int i = 0; i < n_inliers; ++i) {
                        std::cerr << "[SFO_debug]   frac_bin[" << inlier_indices[i]
                                  << "]=" << inlier_bins[i] << "\n";
                    }
                }

                // Correct the OLS slope for Dirichlet-kernel interpolation bias.
                //
                // Log-parabolic interpolation on a Dirichlet (sinc²) peak
                // systematically underestimates fractional bin offsets.
                // The bias depends on the absolute fractional offset (dominated
                // by CFO).  The mapping true→measured is:
                //   δ̂(Δ) = ln((1-Δ)/(1+Δ)) / (2 ln(Δ²/(1-Δ²)))
                // The measured slope ≈ f'(x₀) × true_slope, where x₀ is the
                // average fractional offset.  We correct by dividing slope
                // by f'(x₀), estimated from the OLS intercept.
                double corrected_slope = slope;
                {
                    // Invert the interpolation bias to get true fractional
                    // offset from the OLS intercept.
                    auto fwd = [](double d) -> double {
                        if (d < 1e-12) return 0.0;
                        if (d >= 0.5)  return 0.5;
                        double lp = -2.0 * std::log(1.0 + d);
                        double lc = -2.0 * std::log(d);
                        double ln = -2.0 * std::log(1.0 - d);
                        double den = lp - 2.0 * lc + ln;
                        return (std::abs(den) > 1e-15)
                                   ? 0.5 * (lp - ln) / den
                                   : 0.0;
                    };
                    double meas_avg = std::abs(intercept);
                    // Newton-Raphson: find x such that fwd(x) = meas_avg
                    double x0 = std::min(meas_avg * 3.0, 0.499);
                    for (int iter = 0; iter < 6; ++iter) {
                        double fx = fwd(x0);
                        constexpr double eps = 1e-7;
                        double dfx = (fwd(x0 + eps) - fwd(x0 - eps)) /
                                     (2.0 * eps);
                        if (std::abs(dfx) < 1e-15) break;
                        x0 -= (fx - meas_avg) / dfx;
                        x0 = std::max(1e-6, std::min(0.499, x0));
                    }
                    // f'(x0): derivative of the forward model at the true
                    // operating point.
                    constexpr double eps2 = 1e-7;
                    double fp = (fwd(x0 + eps2) - fwd(x0 - eps2)) /
                                (2.0 * eps2);
                    if (fp > 0.05) {
                        corrected_slope = slope / fp;
                    }
                }

                if (debug_sfo) {
                    std::cerr << "[SFO_debug] corrected_slope=" << corrected_slope
                              << " (raw=" << slope << ")\n";
                }

                // Accept SFO only if:
                // 1. Statistically significant (t > 3.0)
                // 2. Physically plausible (< 100 ppm in bins/symbol)
                // 3. Large enough to matter (> 0.003 bins/symbol)
                // 4. Good linear fit (R² > 0.8) to reject noise artifacts
                const double max_sfo_bins = 100.0e-6 * n_bins_;  // 100 ppm
                if (t_stat > 3.0 && std::abs(corrected_slope) < max_sfo_bins
                    && std::abs(corrected_slope) > 0.003 && r_squared > 0.8) {
                    estimate.sfo_slope = static_cast<float>(corrected_slope);
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
