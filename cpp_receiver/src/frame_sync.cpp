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
#include <utility>
#include <unordered_map>
#include <vector>

// Frame synchronization is the trickiest stage in the LoRa PHY because it has
// to line up timing and coarse frequency from raw captures that may have large
// CFO and front-end artifacts. This file contains both the offline
// `FrameSynchronizer` (single vector) and the stateful `StreamingFrameSynchronizer`
// used by the streaming receiver. The implementation mirrors SDR references but
// is heavily annotated to explain each heuristic so future tweaks stay safe.

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

// Wrap value into [0, period) and then shift to a symmetric interval around 0 later as needed.
double wrap_mod(double value, double period) {
    double result = std::fmod(value, period);
    if (result < 0.0) {
        result += period;
    }
    return result;
}

// Return the index of the element with maximum magnitude.
template <typename Complex>
std::size_t argmax_abs(const std::vector<Complex> &vec) {
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
    // These are deterministic sequences constructed from SF, BW, and Fs.
    upchirp_ = make_upchirp(sf_, bandwidth_hz_, sample_rate_hz_);
    downchirp_ = make_downchirp(sf_, bandwidth_hz_, sample_rate_hz_);
    upchirp_f_.resize(upchirp_.size());
    downchirp_f_.resize(downchirp_.size());
    for (std::size_t i = 0; i < upchirp_.size(); ++i) {
        upchirp_f_[i] = std::complex<float>(static_cast<float>(upchirp_[i].real()),
                                            static_cast<float>(upchirp_[i].imag()));
    }
    for (std::size_t i = 0; i < downchirp_.size(); ++i) {
        downchirp_f_[i] = std::complex<float>(static_cast<float>(downchirp_[i].real()),
                                              static_cast<float>(downchirp_[i].imag()));
    }
}

StreamingFrameSynchronizer::StreamingFrameSynchronizer(int sf, int bandwidth_hz, int sample_rate_hz)
    : base_(sf, bandwidth_hz, sample_rate_hz) {
    // Cache samples-per-symbol for trimming and guard calculations.
    sps_ = base_.samples_per_symbol();
}

void StreamingFrameSynchronizer::reset() {
    // Return to a clean state; drop buffer and any active detection lock.
    buffer_.clear();
    buffer_global_offset_ = 0;
    total_samples_ingested_ = 0;
    locked_ = false;
    detection_.reset();
}

void StreamingFrameSynchronizer::prime(std::span<const Sample> samples, std::size_t global_offset) {
    // Initialize the rolling buffer with a baseline chunk and set the absolute
    // sample index of its first element to `global_offset`.
    reset();
    buffer_global_offset_ = global_offset;
    append_samples(samples);
}

std::optional<FrameSyncResult> StreamingFrameSynchronizer::update(std::span<const Sample> chunk) {
    if (chunk.empty()) {
        return detection_;
    }
    append_samples(chunk);

    if (!locked_) {
        // Try to detect a new preamble in the current buffer. When a detection
        // exists we keep the buffer (so downstream decoders can consume it).
        // Otherwise, we trim to bound memory.
        detection_ = base_.synchronize(buffer_, scratch_frame_);
        if (detection_.has_value()) {
            locked_ = true;
        } else {
            trim_buffer();
        }
    }

    return detection_;
}

void StreamingFrameSynchronizer::append_samples(std::span<const Sample> samples) {
    // Extend the rolling buffer and track the total number of samples ingested.
    buffer_.insert(buffer_.end(), samples.begin(), samples.end());
    total_samples_ingested_ += samples.size();
}

std::size_t StreamingFrameSynchronizer::guard_keep_samples() const {
    // Keep a safety margin of symbols in the buffer to allow look-back for
    // fine searches without reloading older samples.
    return 16 * sps_;
}

void StreamingFrameSynchronizer::trim_buffer() {
    const std::size_t guard_keep = guard_keep_samples();
    if (buffer_.size() > guard_keep) {
        consume(buffer_.size() - guard_keep);
    }
}

void StreamingFrameSynchronizer::consume(std::size_t samples) {
    if (samples == 0 || samples > buffer_.size()) {
        return;
    }
    // Physically remove the consumed prefix and advance the absolute index.
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(samples));
    buffer_global_offset_ += samples;
    if (locked_) {
        // Consuming invalidates any previous detection because offsets change
        // and the detection should be re-established by the caller.
        locked_ = false;
        detection_.reset();
    }
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
    Scratch scratch;
    return synchronize(std::span<const Sample>(samples.data(), samples.size()), scratch);
}

std::optional<FrameSyncResult> FrameSynchronizer::synchronize(std::span<const Sample> samples,
                                                             Scratch &scratch) const {
    const std::size_t N = sps_;
    if (samples.size() < N) {
        return std::nullopt;
    }

    const std::size_t chips = static_cast<std::size_t>(1) << sf_;
    const double fs = static_cast<double>(sample_rate_hz_);
    const std::size_t Nrise = static_cast<std::size_t>(std::ceil(kTriseSeconds * fs));

    // History buffers for recent peak metrics per phase and chirp orientation.
    std::vector<std::array<double, 6>> m_vec(2 * kPhases);
    auto &phase_history = scratch.ensure_phase_history(m_vec.size() * 6);
    for (std::size_t i = 0; i < m_vec.size(); ++i) {
        for (std::size_t j = 0; j < 6; ++j) {
            m_vec[i][j] = phase_history[i * 6 + j];
        }
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
        // Dechirp windows directly into float scratch buffers to avoid double temporaries.
        auto &Su = scratch.ensure_spectrum_up_float(N);
        auto &Sd = scratch.ensure_spectrum_down_float(N);
        Su.resize(N);
        Sd.resize(N);
        for (std::size_t i = 0; i < N; ++i) {
            const auto sample = samples[s_ofs + i];
            Su[i] = sample * downchirp_f_[i];
            Sd[i] = sample * upchirp_f_[i];
        }
        // 2) Coarse DFT and peak bin selection for both orientations.
        lora::fft::transform_pow2(Su, /*inverse=*/false, scratch.coarse_fft_scratch);
        lora::fft::transform_pow2(Sd, /*inverse=*/false, scratch.coarse_fft_scratch);

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
                    // Fine-grain dechirp buffer reused to avoid reallocations when scratch is provided.
                    auto &spec = scratch.ensure_fine_segment_float(N);
                    spec.resize(N);
                    for (std::size_t n = 0; n < N; ++n) {
                        spec[n] = samples[static_cast<std::size_t>(start) + n] * downchirp_f_[n];
                    }
                    lora::fft::transform_pow2(spec, /*inverse=*/false, scratch.fine_fft_scratch);
                    const std::size_t idx = argmax_abs(spec);
                    const auto mag_at = [&](std::ptrdiff_t offset) -> double {
                        std::ptrdiff_t wrapped = static_cast<std::ptrdiff_t>(idx) + offset;
                        if (wrapped < 0) {
                            wrapped += static_cast<std::ptrdiff_t>(N);
                        } else if (wrapped >= static_cast<std::ptrdiff_t>(N)) {
                            wrapped -= static_cast<std::ptrdiff_t>(N);
                        }
                        return std::abs(spec[static_cast<std::size_t>(wrapped)]);
                    };
                    const double ym1 = mag_at(-1);
                    const double y0 = mag_at(0);
                    const double yp1 = mag_at(1);
                    const double denom = ym1 - 2.0 * y0 + yp1;
                    double delta = 0.0;
                    if (std::abs(denom) > 1e-9) {
                        delta = 0.5 * (ym1 - yp1) / denom;
                    }
                    double peak = (static_cast<double>(idx) + delta) * static_cast<double>(kFineOversample);
                    peak = wrap_mod(peak - static_cast<double>(kFineOversample) + fine_period / 2.0, fine_period) - fine_period / 2.0;
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
                    auto &spec = scratch.ensure_fine_segment_float(N);
                    spec.resize(N);
                    for (std::size_t n = 0; n < N; ++n) {
                        spec[n] = samples[static_cast<std::size_t>(start) + n] * upchirp_f_[n];
                    }
                    lora::fft::transform_pow2(spec, /*inverse=*/false, scratch.fine_fft_scratch);
                    const std::size_t idx = argmax_abs(spec);
                    const auto mag_at = [&](std::ptrdiff_t offset) -> double {
                        std::ptrdiff_t wrapped = static_cast<std::ptrdiff_t>(idx) + offset;
                        if (wrapped < 0) {
                            wrapped += static_cast<std::ptrdiff_t>(N);
                        } else if (wrapped >= static_cast<std::ptrdiff_t>(N)) {
                            wrapped -= static_cast<std::ptrdiff_t>(N);
                        }
                        return std::abs(spec[static_cast<std::size_t>(wrapped)]);
                    };
                    const double ym1 = mag_at(-1);
                    const double y0 = mag_at(0);
                    const double yp1 = mag_at(1);
                    const double denom = ym1 - 2.0 * y0 + yp1;
                    double delta = 0.0;
                    if (std::abs(denom) > 1e-9) {
                        delta = 0.5 * (ym1 - yp1) / denom;
                    }
                    double peak = (static_cast<double>(idx) + delta) * static_cast<double>(kFineOversample);
                    peak = wrap_mod(peak - static_cast<double>(kFineOversample) + fine_period / 2.0, fine_period) - fine_period / 2.0;
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

    // Round up to the next integer sample to avoid early indexing before the estimated start.
    const std::ptrdiff_t p_ofs_est = static_cast<std::ptrdiff_t>(std::ceil(t_est));

    // Package results. preamble_offset is clamped to non-negative.
    FrameSyncResult result;
    result.p_ofs_est = p_ofs_est;
    // The coarse preamble estimate is derived from the window start minus a
    // heuristic number of symbols traversed during detection (aligned with the
    // pattern check above). Clamp negative to 0 relative to the provided buffer.
    const std::ptrdiff_t preamble = static_cast<std::ptrdiff_t>(best_s_ofs) -
                                    static_cast<std::ptrdiff_t>(11 * N);
    result.preamble_offset = preamble > 0 ? preamble : 0;
    result.cfo_hz = cfo_est;

    if (const auto sr = estimate_sample_rate(samples, result.preamble_offset, scratch)) {
        result.sample_rate_ratio = sr->ratio;
        result.sample_rate_error_ppm = sr->ppm_error;
        result.sample_rate_drift_per_symbol = sr->drift_per_symbol;
    }
    return result;
}

std::optional<FrameSynchronizer::SampleRateEstimate>
FrameSynchronizer::estimate_sample_rate(std::span<const Sample> samples,
                                        std::ptrdiff_t preamble_offset,
                                        Scratch &scratch) const {
    if (preamble_offset < 0 || sps_ == 0) {
        return std::nullopt;
    }

    const std::size_t N = sps_;
    const double total_samples = static_cast<double>(samples.size());
    const double base = static_cast<double>(preamble_offset);
    if (base + static_cast<double>(N) + 2.0 >= total_samples) {
        return std::nullopt;
    }

    constexpr double kSearchRange = 2.5;
    const double min_index = std::max(0.0, std::floor(base - kSearchRange - 1.0));
    const double max_symbol = 7.0; // highest symbol inspected in kSymbolPairs
    double max_index = base + (max_symbol + 1.0) * static_cast<double>(N) + kSearchRange + 2.0;
    if (max_index > total_samples) {
        max_index = total_samples;
    }
    const std::size_t buffer_start = static_cast<std::size_t>(min_index);
    std::size_t buffer_end = static_cast<std::size_t>(std::ceil(max_index));
    if (buffer_end + 2 > samples.size()) {
        buffer_end = samples.size() > 2 ? samples.size() - 2 : 0;
    }
    const std::size_t buffer_length = (buffer_end + 2 > buffer_start) ? (buffer_end + 2 - buffer_start) : 0;

    if (buffer_length == 0) {
        return std::nullopt;
    }

    auto &sr_buffer = scratch.ensure_sr_samples(buffer_length);
    for (std::size_t i = 0; i < buffer_length; ++i) {
        const auto &sample = samples[buffer_start + i];
        sr_buffer[i] = CDouble(static_cast<double>(sample.real()), static_cast<double>(sample.imag()));
    }
    const CDouble *sr_data = sr_buffer.data();
    const std::size_t sr_size = sr_buffer.size();
    const double buffer_start_d = static_cast<double>(buffer_start);

    const CDouble *downchirp_ptr = downchirp_.data();
    static constexpr std::array<int, 6> kShiftCandidates{{-3, -2, -1, 0, 1, 2}};
    static constexpr int kShiftMin = kShiftCandidates.front();
    static constexpr int kShiftMax = kShiftCandidates.back();
    static constexpr std::size_t kShiftCount = kShiftCandidates.size();

    const auto estimate_delta = [&](std::size_t symbol_index, double &delta_out) -> bool {
        const double expected_start = base + static_cast<double>(symbol_index) * static_cast<double>(N);
        if (expected_start - kSearchRange < 0.0 ||
            expected_start + static_cast<double>(N) + kSearchRange + 1.0 >= total_samples) {
            return false;
        }

        struct DechirpSlice {
            std::complex<double> *main = nullptr;
            std::complex<double> *next = nullptr;
        };

        auto &cache_main = scratch.ensure_sr_dechirp_main(kShiftCount * N);
        auto &cache_next = scratch.ensure_sr_dechirp_next(kShiftCount * N);
        std::array<DechirpSlice, kShiftCount> shift_views{};

        for (std::size_t shift_idx = 0; shift_idx < kShiftCount; ++shift_idx) {
            const int shift = kShiftCandidates[shift_idx];
            const double start = expected_start + static_cast<double>(shift);
            if (start < 0.0 || start + static_cast<double>(N) + 1.0 >= total_samples) {
                return false;
            }
            const double rel = start - buffer_start_d;
            if (rel < 0.0) {
                return false;
            }
            const auto base_index = static_cast<std::size_t>(rel);
            if (base_index + N >= sr_size) {
                return false;
            }

            auto *main_ptr = cache_main.data() + shift_idx * N;
            auto *next_ptr = cache_next.data() + shift_idx * N;
            for (std::size_t n = 0; n < N; ++n) {
                const std::size_t sample_index = base_index + n;
                const std::size_t next_sample_index = sample_index + 1;
                if (next_sample_index >= sr_size) {
                    return false;
                }
                main_ptr[n] = sr_data[sample_index] * downchirp_ptr[n];
                next_ptr[n] = sr_data[next_sample_index] * downchirp_ptr[n];
            }
            shift_views[shift_idx] = DechirpSlice{main_ptr, next_ptr};
        }

        std::unordered_map<long long, double> metric_cache;
        metric_cache.reserve(128);
        const auto evaluate_metric = [&](double delta, double &metric_out) -> bool {
            const long long key = static_cast<long long>(std::llround(delta * 100.0));
            auto it = metric_cache.find(key);
            if (it != metric_cache.end()) {
                metric_out = it->second;
                return true;
            }
            const double cursor_begin = expected_start + delta;
            if (cursor_begin < 0.0 || cursor_begin + static_cast<double>(N) + 1.0 >= total_samples) {
                return false;
            }
            const double shift_floor = std::floor(delta);
            const int shift = static_cast<int>(shift_floor);
            if (shift < kShiftMin || shift > kShiftMax) {
                return false;
            }
            const std::size_t shift_index = static_cast<std::size_t>(shift - kShiftMin);
            auto frac = delta - shift_floor;
            if (frac < 0.0) {
                if (frac > -1e-9) {
                    frac = 0.0;
                } else {
                    return false;
                }
            } else if (frac >= 1.0) {
                if (frac < 1.0 + 1e-9) {
                    frac = 1.0 - 1e-9;
                } else {
                    return false;
                }
            }
            const double w0 = 1.0 - frac;
            const double w1 = frac;
            const auto *main_ptr = shift_views[shift_index].main;
            const auto *next_ptr = shift_views[shift_index].next;
            if (main_ptr == nullptr || next_ptr == nullptr) {
                return false;
            }

            std::complex<double> acc{0.0, 0.0};
            for (std::size_t n = 0; n < N; ++n) {
                acc += w0 * main_ptr[n] + w1 * next_ptr[n];
            }
            metric_out = std::norm(acc);
            metric_cache.emplace(key, metric_out);
            return true;
        };

        constexpr double kCoarseStep = 0.5;
        double best_delta = 0.0;
        double best_metric = -1.0;
        for (double delta = -kSearchRange; delta <= kSearchRange; delta += kCoarseStep) {
            double metric = 0.0;
            if (evaluate_metric(delta, metric) && metric > best_metric) {
                best_metric = metric;
                best_delta = delta;
            }
        }
        if (best_metric < 0.0) {
            return false;
        }

        const std::array<std::pair<double, double>, 2> refinements{{{0.25, 0.05}, {0.08, 0.01}}};
        for (const auto &[range, step] : refinements) {
            double local_best_delta = best_delta;
            double local_best_metric = best_metric;
            for (double delta = best_delta - range; delta <= best_delta + range; delta += step) {
                double metric = 0.0;
                if (evaluate_metric(delta, metric) && metric > local_best_metric) {
                    local_best_metric = metric;
                    local_best_delta = delta;
                }
            }
            best_delta = local_best_delta;
            best_metric = local_best_metric;
        }

        delta_out = best_delta;
        return true;
    };

    static constexpr std::pair<std::size_t, std::size_t> kSymbolPairs[] = {
        {1, 7},
        {0, 6},
    };

    for (const auto &[first_symbol, last_symbol] : kSymbolPairs) {
        if (last_symbol <= first_symbol) {
            continue;
        }
        double delta_first = 0.0;
        double delta_last = 0.0;
        if (!estimate_delta(first_symbol, delta_first) ||
            !estimate_delta(last_symbol, delta_last)) {
            continue;
        }
        const double symbols_between = static_cast<double>(last_symbol - first_symbol);
        const double drift_per_symbol = (delta_last - delta_first) / symbols_between;
        const double ratio = 1.0 + drift_per_symbol / static_cast<double>(N);
        if (ratio <= 0.0 || !std::isfinite(ratio)) {
            continue;
        }
        SampleRateEstimate estimate;
        estimate.ratio = ratio;
        estimate.drift_per_symbol = drift_per_symbol;
        estimate.ppm_error = (ratio - 1.0) * 1e6;
        return estimate;
    }

    return std::nullopt;
}

#ifdef LORA_EMBEDDED_PROFILE
FrameSynchronizer::MutableComplexSpan FrameSynchronizer::default_win_up(std::vector<std::complex<double>> &buffer, std::size_t size) {
    buffer.resize(size);
    return MutableComplexSpan{buffer.data(), buffer.size()};
}

FrameSynchronizer::MutableComplexSpan FrameSynchronizer::default_win_down(std::vector<std::complex<double>> &buffer, std::size_t size) {
    buffer.resize(size);
    return MutableComplexSpan{buffer.data(), buffer.size()};
}

FrameSynchronizer::MutableDoubleSpan FrameSynchronizer::default_phase_history(std::vector<double> &buffer, std::size_t size) {
    buffer.resize(size);
    return MutableDoubleSpan{buffer.data(), buffer.size()};
}
#endif

} // namespace lora
