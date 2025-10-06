#include "frame_sync.hpp"

#include "chirp_generator.hpp"
#include "fft_utils.hpp"

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

// Symbol rise time used to compensate timing bias on the first useful symbol
// (heuristic delay from analog front-end or windowing used by the reference).
constexpr double kTriseSeconds = 50e-6;

// Number of interleaved phases to scan per symbol. The code advances by N/kPhases
// samples each iteration, alternating phase groups to detect the expected preamble pattern.
constexpr std::size_t kPhases = 2;

// Fine search FFT oversampling factor used for sub-bin peak interpolation.
constexpr std::size_t kFineOversample = 4;

using CDouble = std::complex<double>;

// Convert an input sample (likely float or int16 in the typedef) to std::complex<double>.
CDouble to_cdouble(const FrameSynchronizer::Sample &s) {
    return CDouble(static_cast<double>(s.real()), static_cast<double>(s.imag()));
}

// FFT-based spectrum utility with optional zero-padding to fft_len.
// Uses lora::fft::transform_pow2 and returns the in-place spectrum copy.
std::vector<CDouble> compute_spectrum_fft(const std::vector<CDouble> &input, std::size_t fft_len, bool inverse = false) {
    std::vector<CDouble> spectrum = input;
    spectrum.resize(fft_len, CDouble{0.0, 0.0});
    lora::fft::transform_pow2(spectrum, inverse);
    return spectrum;
}

// Wrap value into [0, period) and then shift to a symmetric interval around 0 later as needed.
double wrap_mod(double value, double period) {
    double result = std::fmod(value, period);
    if (result < 0.0) {
        result += period;
    }
    return result;
}

// Return the index of the element with maximum magnitude.
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

// FrameSynchronizer
// Responsibilities:
//  - Generate reference up/down chirps for the given LoRa parameters.
//  - Slide over the input samples by N/kPhases and detect the preamble/sync pattern.
//  - Perform coarse peak checks and, upon a plausible match, run a fine search to estimate:
//      * CFO (carrier frequency offset)
//      * Symbol timing / preamble start offset
//  - Return FrameSyncResult with estimated preamble offset and CFO.
FrameSynchronizer::FrameSynchronizer(int sf, int bandwidth_hz, int sample_rate_hz)
    : sf_(sf), bandwidth_hz_(bandwidth_hz), sample_rate_hz_(sample_rate_hz) {
    // Validate LoRa parameters and integer oversampling assumption (Fs is a multiple of BW).
    if (sf < 5 || sf > 12) {
        throw std::invalid_argument("Spreading factor out of supported range (5-12)");
    }
    if (bandwidth_hz <= 0 || sample_rate_hz <= 0) {
        throw std::invalid_argument("Bandwidth and sample rate must be positive");
    }
    if (sample_rate_hz % bandwidth_hz != 0) {
        throw std::invalid_argument("Sample rate must be an integer multiple of bandwidth for integer oversampling");
    }

    // Derived sizing: samples per symbol (sps) and precomputed reference chirps.
    os_factor_ = static_cast<std::size_t>(sample_rate_hz_) / static_cast<std::size_t>(bandwidth_hz_);
    const std::size_t chips_per_symbol = static_cast<std::size_t>(1) << sf_;
    sps_ = chips_per_symbol * os_factor_;

    // Reference chirps used for dechirp (down) and matched filtering / checks (up).
    upchirp_ = make_upchirp(sf_, bandwidth_hz_, sample_rate_hz_);
    downchirp_ = make_downchirp(sf_, bandwidth_hz_, sample_rate_hz_);
}

// Slide a one-symbol window across the stream to detect the LoRa preamble pattern.
// Steps per iteration:
//  1) Dechirp window with downchirp (for up-chirp detection) and with upchirp (for down-chirp detection).
//  2) DFT both windows; take bin indices of the largest magnitudes (coarse peaks).
//  3) Track recent peak positions per phase and test a pattern consistent with LoRa preamble.
//  4) On a plausible match, run a finer search using zero-padded/oversampled DFT around several symbols
//     and parabolic interpolation to refine fractional-bin peaks (sub-bin accuracy).
//  5) Convert refined peaks into CFO estimate and timing estimate; return preamble offset and CFO.
std::optional<FrameSyncResult> FrameSynchronizer::synchronize(const std::vector<Sample> &samples) const {
    const std::size_t N = sps_;
    if (samples.size() < N) {
        return std::nullopt;
    }

    const std::size_t chips = static_cast<std::size_t>(1) << sf_;
    const double fs = static_cast<double>(sample_rate_hz_);
    const std::size_t Nrise = static_cast<std::size_t>(std::ceil(kTriseSeconds * fs));

    // History buffers for recent peak metrics per phase and chirp orientation.
    std::vector<std::array<double, 6>> m_vec(2 * kPhases);
    for (auto &arr : m_vec) {
        arr.fill(-1.0);
    }

    std::size_t s_ofs = 0;      // current sample offset of the window start
    std::size_t m_phase = 0;    // phase index in [0, kPhases)
    bool found = false;         // set true when a plausible preamble is detected and refined
    double best_metric = std::numeric_limits<double>::infinity();
    std::size_t best_s_ofs = 0; // best window start for the detection
    double best_m_u0 = 0.0;     // refined up-chirp peak accumulator
    double best_m_d0 = 0.0;     // refined down-chirp peak accumulator

    const std::size_t step = N / kPhases; // slide half-symbol per iteration with kPhases=2

    while (s_ofs + N <= samples.size()) {
        // 1) Dechirp current window with reference up/down to convert chirp to near-tones.
        std::vector<CDouble> win_u(N);
        std::vector<CDouble> win_d(N);
        for (std::size_t i = 0; i < N; ++i) {
            const auto cx = to_cdouble(samples[s_ofs + i]);
            win_u[i] = cx * downchirp_[i]; // up-chirp becomes a tone after multiplying by down-chirp
            win_d[i] = cx * upchirp_[i];   // down-chirp becomes a tone after multiplying by up-chirp
        }

        // 2) Coarse DFT and peak bin selection for both orientations.
    const auto Su = compute_spectrum_fft(win_u, N, /*inverse=*/false);
    const auto Sd = compute_spectrum_fft(win_d, N, /*inverse=*/false);

        std::size_t idx_u = argmax_abs(Su);
        std::size_t idx_d = argmax_abs(Sd);

        // Center indices to a symmetric interval around 0 for easier differencing.
        double m_u = wrap_mod(static_cast<double>(idx_u) - 1.0 + static_cast<double>(N) / 2.0,
                              static_cast<double>(N)) - static_cast<double>(N) / 2.0;
        double m_d = wrap_mod(static_cast<double>(idx_d) - 1.0 + static_cast<double>(N) / 2.0,
                              static_cast<double>(N)) - static_cast<double>(N) / 2.0;

        // Maintain a short history (6 entries) per phase and orientation.
        auto &vec_u = m_vec[m_phase * 2];
        auto &vec_d = m_vec[m_phase * 2 + 1];
        for (std::size_t shift_idx = vec_u.size() - 1; shift_idx > 0; --shift_idx) {
            vec_u[shift_idx] = vec_u[shift_idx - 1];
            vec_d[shift_idx] = vec_d[shift_idx - 1];
        }
        vec_u[0] = m_u;
        vec_d[0] = m_d;

        // 3) Coarse pattern check: verify expected relative shifts over recent windows.
        //    These tolerances (~1 bin) and offsets (~8 bins) reflect the preamble structure
        //    when stepping by half-symbols and switching orientation between phases.
        const bool condition_ok = (std::abs(vec_d[0] - vec_d[1]) <= 1.0) &&
                                  (std::abs(vec_u[2] - vec_u[3] - 8.0) <= 1.0) &&
                                  (std::abs(vec_u[3] - vec_u[4] - 8.0) <= 1.0) &&
                                  (std::abs(vec_u[4] - vec_u[5]) <= 1.0);

        if (condition_ok && s_ofs >= (6 * N)) {
            // Use a simple metric combining older down-chirp and much older up-chirp peaks; lower is better.
            const double tmp = std::abs(vec_d[1]) + std::abs(vec_u[5]);
            if (tmp < best_metric) {
                best_metric = tmp;

                // 4) Fine search: refine peak positions with kFineOversample zero-padding and
                //    3-point parabolic interpolation for sub-bin accuracy.
                double m_u0 = 0.0;
                const double fine_period = static_cast<double>(N * kFineOversample);
                bool fine_valid = true;
                // Look back a few symbols in the up-chirp domain.
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
                    const auto spec = compute_spectrum_fft(seg, N * kFineOversample, /*inverse=*/false);
                    std::size_t idx = argmax_abs(spec);
                    double peak = static_cast<double>(idx);
                    // Parabolic interpolation using neighbors to estimate fractional peak position.
                    if (idx > 0 && idx + 1 < spec.size()) {
                        const double ym1 = std::abs(spec[idx - 1]);
                        const double y0 = std::abs(spec[idx]);
                        const double yp1 = std::abs(spec[idx + 1]);
                        const double denom = ym1 - 2.0 * y0 + yp1;
                        if (std::abs(denom) > 1e-9) {
                            peak += 0.5 * (ym1 - yp1) / denom;
                        }
                    }
                    // Center to symmetric interval around 0.
                    peak = wrap_mod(peak - 1.0 + fine_period / 2.0, fine_period) - fine_period / 2.0;
                    m_u0 += peak;
                }
                if (!fine_valid) {
                    break;
                }
                m_u0 /= 2.0;

                double m_d0 = 0.0;
                // Use recent symbols in the down-chirp domain for symmetry.
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
                    const auto spec = compute_spectrum_fft(seg, N * kFineOversample, /*inverse=*/false);
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

                // Persist the best estimates so far.
                best_s_ofs = s_ofs;
                best_m_u0 = m_u0;
                best_m_d0 = m_d0;
                found = true;
            }
        }

        // Move to next phase/offset.
        m_phase = (m_phase + 1) % kPhases;
        s_ofs += step;
    }

    if (!found) {
        return std::nullopt;
    }

    // 5) Convert sub-bin peaks to CFO (Hz) and timing (samples).
    // CFO estimate: average of up/down refined peaks scaled by BW/chips and oversampling.
    const double cfo_est = (best_m_u0 + best_m_d0) / 2.0 * static_cast<double>(bandwidth_hz_) /
                           static_cast<double>(chips) / static_cast<double>(kFineOversample);

    // Timing estimate (samples): combine difference of peaks with oversampling to get fractional timing,
    // add the coarse window start, and compensate for preamble length and frontend rise time.
    const double t_est = (best_m_d0 - best_m_u0) * static_cast<double>(os_factor_) /
                         (2.0 * static_cast<double>(kFineOversample)) + static_cast<double>(best_s_ofs) -
                         11.0 * static_cast<double>(N) - static_cast<double>(Nrise);

    const std::ptrdiff_t p_ofs_est = static_cast<std::ptrdiff_t>(std::ceil(t_est));

    // Package results. preamble_offset is clamped to non-negative.
    FrameSyncResult result;
    result.p_ofs_est = p_ofs_est;
    const std::ptrdiff_t preamble = static_cast<std::ptrdiff_t>(best_s_ofs) -
                                    static_cast<std::ptrdiff_t>(11 * N);
    result.preamble_offset = preamble > 0 ? preamble : 0;
    result.cfo_hz = cfo_est;
    return result;
}

} // namespace lora
