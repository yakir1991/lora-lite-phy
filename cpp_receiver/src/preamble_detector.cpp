#include "preamble_detector.hpp"

#include "chirp_generator.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

// The preamble detector acts as a coarse entry point to the LoRa frame: it finds
// the repeating up-chirps and hands back an offset plus a simple match metric.
// We keep the implementation intentionally direct (two-pass matched filtering)
// because other stages depend on deterministic behavior for unit tests and CLI
// tooling. Any heuristics or constants are documented to ease future tuning.

namespace lora {

// PreambleDetector performs a simple matched-filter search for the LoRa preamble.
// It correlates a reference up-chirp against the input and finds the offset with
// maximum magnitude (normalized). The search is done in two stages: coarse stride,
// then fine per-sample refinement.
PreambleDetector::PreambleDetector(int sf, int bandwidth_hz, int sample_rate_hz)
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

    const std::size_t os_factor = static_cast<std::size_t>(sample_rate_hz_) / static_cast<std::size_t>(bandwidth_hz_);
    const std::size_t chips_per_symbol = static_cast<std::size_t>(1) << sf_;
    sps_ = chips_per_symbol * os_factor;

    // Precompute the ideal reference up-chirp used for correlation.
    reference_upchirp_ = make_upchirp(sf_, bandwidth_hz_, sample_rate_hz_);
}

// Run matched-filter detection.
// - Coarse pass: step by sps/4 samples and track the best correlation magnitude.
// - Refine pass: search +/- one coarse step around the coarse best with stride=1.
// Metric is |sum(conj(ref[i]) * x[pos+i])| / sps. We pick the maximum; ties break by smaller offset.
std::optional<PreambleDetection> PreambleDetector::detect(const std::vector<Sample> &samples) const {
    Scratch scratch;
    return detect(std::span<const Sample>(samples.data(), samples.size()), scratch);
}

std::optional<PreambleDetection> PreambleDetector::detect(std::span<const Sample> samples,
                                                          Scratch &scratch) const {
    if (samples.size() < sps_) {
        return std::nullopt;
    }

    const std::size_t step = std::max<std::size_t>(1, sps_ / 4);
    std::size_t coarse_best_offset = 0;
    double coarse_best_metric = -1.0;

    // Coarse sweep with larger stride to reduce work.
    for (std::size_t pos = 0; pos + sps_ <= samples.size(); pos += step) {
        std::complex<double> acc{0.0, 0.0};
        for (std::size_t i = 0; i < sps_; ++i) {
            const auto &sample = samples[pos + i];
            acc += std::conj(reference_upchirp_[i]) * std::complex<double>(sample.real(), sample.imag());
        }
        const double metric = std::abs(acc) / static_cast<double>(sps_);
        if (metric > coarse_best_metric + 1e-9) {
            coarse_best_metric = metric;
            coarse_best_offset = pos;
        }
    }

    // Fine search around the best coarse position.
    const std::size_t refine_radius = step;
    const std::size_t start = (coarse_best_offset > refine_radius) ? (coarse_best_offset - refine_radius) : 0;
    const std::size_t end = std::min(samples.size() - sps_, coarse_best_offset + refine_radius);

    std::size_t best_offset = coarse_best_offset;
    double best_metric = coarse_best_metric;

    for (std::size_t pos = start; pos <= end; ++pos) {
        std::complex<double> acc{0.0, 0.0};
        for (std::size_t i = 0; i < sps_; ++i) {
            const auto &sample = samples[pos + i];
            acc += std::conj(reference_upchirp_[i]) * std::complex<double>(sample.real(), sample.imag());
        }
        const double metric = std::abs(acc) / static_cast<double>(sps_);
        // Prefer larger metric; if equal within epsilon, prefer the earlier offset for stability.
        if (metric > best_metric + 1e-9 || (std::abs(metric - best_metric) <= 1e-9 && pos < best_offset)) {
            best_metric = metric;
            best_offset = pos;
        }
    }

    PreambleDetection result;
    result.offset = best_offset;
    result.metric = static_cast<float>(best_metric);
    return result;
}

} // namespace lora
