#include "frame_sync.hpp"

#include "chirp_generator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace lora {

namespace {

constexpr double kTriseSeconds = 50e-6;
constexpr std::size_t kPhases = 2;
constexpr std::size_t kFineOversample = 4;

using CDouble = std::complex<double>;

CDouble to_cdouble(const FrameSynchronizer::Sample &s) {
    return CDouble(static_cast<double>(s.real()), static_cast<double>(s.imag()));
}

std::vector<CDouble> compute_dft(const std::vector<CDouble> &input, std::size_t fft_len, bool inverse = false) {
    std::vector<CDouble> spectrum(fft_len, CDouble{0.0, 0.0});
    const double sign = inverse ? 1.0 : -1.0;
    for (std::size_t k = 0; k < fft_len; ++k) {
        CDouble acc{0.0, 0.0};
        const double coeff = sign * 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(fft_len);
        for (std::size_t n = 0; n < input.size(); ++n) {
            const double angle = coeff * static_cast<double>(n);
            acc += input[n] * CDouble(std::cos(angle), std::sin(angle));
        }
        spectrum[k] = acc;
    }
    return spectrum;
}

double wrap_mod(double value, double period) {
    double result = std::fmod(value, period);
    if (result < 0.0) {
        result += period;
    }
    return result;
}

std::size_t argmax_abs(const std::vector<CDouble> &vec) {
    std::size_t idx = 0;
    double max_mag = 0.0;
    for (std::size_t i = 0; i < vec.size(); ++i) {
        const double mag = std::abs(vec[i]);
        if (mag > max_mag) {
            max_mag = mag;
            idx = i;
        }
    }
    return idx;
}

} // namespace

FrameSynchronizer::FrameSynchronizer(int sf, int bandwidth_hz, int sample_rate_hz)
    : sf_(sf), bandwidth_hz_(bandwidth_hz), sample_rate_hz_(sample_rate_hz) {
    if (sf < 5 || sf > 12) {
        throw std::invalid_argument("Spreading factor out of supported range (5-12)");
    }
    if (bandwidth_hz <= 0 || sample_rate_hz <= 0) {
        throw std::invalid_argument("Bandwidth and sample rate must be positive");
    }
    if (sample_rate_hz % bandwidth_hz != 0) {
        throw std::invalid_argument("Sample rate must be an integer multiple of bandwidth for integer oversampling");
    }

    os_factor_ = static_cast<std::size_t>(sample_rate_hz_) / static_cast<std::size_t>(bandwidth_hz_);
    const std::size_t chips_per_symbol = static_cast<std::size_t>(1) << sf_;
    sps_ = chips_per_symbol * os_factor_;

    upchirp_ = make_upchirp(sf_, bandwidth_hz_, sample_rate_hz_);
    downchirp_ = make_downchirp(sf_, bandwidth_hz_, sample_rate_hz_);
}

std::optional<FrameSyncResult> FrameSynchronizer::synchronize(const std::vector<Sample> &samples) const {
    const std::size_t N = sps_;
    if (samples.size() < N) {
        return std::nullopt;
    }

    const std::size_t chips = static_cast<std::size_t>(1) << sf_;
    const double fs = static_cast<double>(sample_rate_hz_);
    const std::size_t Nrise = static_cast<std::size_t>(std::ceil(kTriseSeconds * fs));

    std::vector<std::array<double, 6>> m_vec(2 * kPhases);
    for (auto &arr : m_vec) {
        arr.fill(-1.0);
    }

    std::size_t s_ofs = 0;
    std::size_t m_phase = 0;
    bool found = false;
    double best_metric = std::numeric_limits<double>::infinity();
    std::size_t best_s_ofs = 0;
    double best_m_u0 = 0.0;
    double best_m_d0 = 0.0;

    const std::size_t step = N / kPhases;

    while (s_ofs + N <= samples.size()) {
        std::vector<CDouble> win_u(N);
        std::vector<CDouble> win_d(N);
        for (std::size_t i = 0; i < N; ++i) {
            const auto cx = to_cdouble(samples[s_ofs + i]);
            win_u[i] = cx * downchirp_[i];
            win_d[i] = cx * upchirp_[i];
        }

        const auto Su = compute_dft(win_u, N, false);
        const auto Sd = compute_dft(win_d, N, false);

        std::size_t idx_u = argmax_abs(Su);
        std::size_t idx_d = argmax_abs(Sd);

        double m_u = wrap_mod(static_cast<double>(idx_u) - 1.0 + static_cast<double>(N) / 2.0,
                              static_cast<double>(N)) - static_cast<double>(N) / 2.0;
        double m_d = wrap_mod(static_cast<double>(idx_d) - 1.0 + static_cast<double>(N) / 2.0,
                              static_cast<double>(N)) - static_cast<double>(N) / 2.0;

        auto &vec_u = m_vec[m_phase * 2];
        auto &vec_d = m_vec[m_phase * 2 + 1];
        for (std::size_t shift_idx = vec_u.size() - 1; shift_idx > 0; --shift_idx) {
            vec_u[shift_idx] = vec_u[shift_idx - 1];
            vec_d[shift_idx] = vec_d[shift_idx - 1];
        }
        vec_u[0] = m_u;
        vec_d[0] = m_d;

        const bool condition_ok = (std::abs(vec_d[0] - vec_d[1]) <= 1.0) &&
                                  (std::abs(vec_u[2] - vec_u[3] - 8.0) <= 1.0) &&
                                  (std::abs(vec_u[3] - vec_u[4] - 8.0) <= 1.0) &&
                                  (std::abs(vec_u[4] - vec_u[5]) <= 1.0);

        if (condition_ok && s_ofs >= (6 * N)) {
            const double tmp = std::abs(vec_d[1]) + std::abs(vec_u[5]);
            if (tmp < best_metric) {
                best_metric = tmp;

                double m_u0 = 0.0;
                const double fine_period = static_cast<double>(N * kFineOversample);
                bool fine_valid = true;
                for (std::size_t i = 1; i <= 2; ++i) {
                    const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(s_ofs) - static_cast<std::ptrdiff_t>((4 + i) * N);
                    if (start < 0) {
                        fine_valid = false;
                        break;
                    }
                    std::vector<CDouble> seg(N);
                    for (std::size_t n = 0; n < N; ++n) {
                        seg[n] = to_cdouble(samples[static_cast<std::size_t>(start) + n]) * downchirp_[n];
                    }
                    const auto spec = compute_dft(seg, N * kFineOversample, false);
                    std::size_t idx = argmax_abs(spec);
                    double peak = static_cast<double>(idx);
                    if (idx > 0 && idx + 1 < spec.size()) {
                        const double ym1 = std::abs(spec[idx - 1]);
                        const double y0 = std::abs(spec[idx]);
                        const double yp1 = std::abs(spec[idx + 1]);
                        const double denom = ym1 - 2.0 * y0 + yp1;
                        if (std::abs(denom) > 1e-9) {
                            peak += 0.5 * (ym1 - yp1) / denom;
                        }
                    }
                    peak = wrap_mod(peak - 1.0 + fine_period / 2.0, fine_period) - fine_period / 2.0;
                    m_u0 += peak;
                }
                if (!fine_valid) {
                    break;
                }
                m_u0 /= 2.0;

                double m_d0 = 0.0;
                for (std::size_t i = 1; i <= 2; ++i) {
                    const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(s_ofs) - static_cast<std::ptrdiff_t>((i - 1) * N);
                    if (start < 0) {
                        fine_valid = false;
                        break;
                    }
                    std::vector<CDouble> seg(N);
                    for (std::size_t n = 0; n < N; ++n) {
                        seg[n] = to_cdouble(samples[static_cast<std::size_t>(start) + n]) * upchirp_[n];
                    }
                    const auto spec = compute_dft(seg, N * kFineOversample, false);
                    std::size_t idx = argmax_abs(spec);
                    double peak = static_cast<double>(idx);
                    if (idx > 0 && idx + 1 < spec.size()) {
                        const double ym1 = std::abs(spec[idx - 1]);
                        const double y0 = std::abs(spec[idx]);
                        const double yp1 = std::abs(spec[idx + 1]);
                        const double denom = ym1 - 2.0 * y0 + yp1;
                        if (std::abs(denom) > 1e-9) {
                            peak += 0.5 * (ym1 - yp1) / denom;
                        }
                    }
                    peak = wrap_mod(peak - 1.0 + fine_period / 2.0, fine_period) - fine_period / 2.0;
                    m_d0 += peak;
                }
                if (!fine_valid) {
                    break;
                }
                m_d0 /= 2.0;

                best_s_ofs = s_ofs;
                best_m_u0 = m_u0;
                best_m_d0 = m_d0;
                found = true;
            }
        }

        m_phase = (m_phase + 1) % kPhases;
        s_ofs += step;
    }

    if (!found) {
        return std::nullopt;
    }

    const double cfo_est = (best_m_u0 + best_m_d0) / 2.0 * static_cast<double>(bandwidth_hz_) /
                           static_cast<double>(chips) / static_cast<double>(kFineOversample);
    const double t_est = (best_m_d0 - best_m_u0) * static_cast<double>(os_factor_) /
                         (2.0 * static_cast<double>(kFineOversample)) + static_cast<double>(best_s_ofs) -
                         11.0 * static_cast<double>(N) - static_cast<double>(Nrise);

    const std::ptrdiff_t p_ofs_est = static_cast<std::ptrdiff_t>(std::ceil(t_est));

    FrameSyncResult result;
    result.p_ofs_est = p_ofs_est;
    const std::ptrdiff_t preamble = static_cast<std::ptrdiff_t>(best_s_ofs) -
                                    static_cast<std::ptrdiff_t>(11 * N);
    result.preamble_offset = preamble > 0 ? preamble : 0;
    result.cfo_hz = cfo_est;
    return result;
}

} // namespace lora
