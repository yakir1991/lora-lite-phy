#include "sync_word_detector.hpp"

#include "chirp_generator.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>

namespace lora {

namespace {

constexpr std::size_t kPreambleSymCount = 8;
constexpr std::size_t kSyncSymCount = 2;

} // namespace

SyncWordDetector::SyncWordDetector(int sf, int bandwidth_hz, int sample_rate_hz, unsigned sync_word)
    : sf_(sf), bandwidth_hz_(bandwidth_hz), sample_rate_hz_(sample_rate_hz), sync_word_(sync_word & 0xFFu) {
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

    downchirp_ = make_downchirp(sf_, bandwidth_hz_, sample_rate_hz_);
}

std::size_t SyncWordDetector::demod_symbol(const std::vector<Sample> &samples,
                                           std::size_t sym_index,
                                           std::ptrdiff_t preamble_offset,
                                           FFTScratch &scratch,
                                           double &magnitude) const {
    const std::ptrdiff_t start_signed = preamble_offset + static_cast<std::ptrdiff_t>(sym_index * sps_);
    if (start_signed < 0) {
        throw std::out_of_range("SyncWordDetector: negative start index");
    }
    const std::size_t start = static_cast<std::size_t>(start_signed);
    scratch.input.assign(sps_, std::complex<double>{});

    for (std::size_t i = 0; i < sps_; ++i) {
        const auto &s = samples[start + i];
        scratch.input[i] = std::complex<double>(s.real(), s.imag()) * downchirp_[i];
    }

    const std::size_t N = scratch.input.size();
    scratch.spectrum.assign(N, std::complex<double>{});
    for (std::size_t k = 0; k < N; ++k) {
        std::complex<double> acc{0.0, 0.0};
        const double coeff = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(N);
        for (std::size_t n = 0; n < N; ++n) {
            const double angle = coeff * static_cast<double>(n);
            const double c = std::cos(angle);
            const double s = std::sin(angle);
            acc += scratch.input[n] * std::complex<double>(c, s);
        }
        scratch.spectrum[k] = acc;
    }

    std::size_t best_bin = 0;
    double best_mag = 0.0;
    for (std::size_t k = 0; k < N; ++k) {
        const double mag = std::abs(scratch.spectrum[k]);
        if (mag > best_mag) {
            best_mag = mag;
            best_bin = k;
        }
    }

    magnitude = best_mag;
    return best_bin;
}

std::optional<SyncWordDetection> SyncWordDetector::analyze(const std::vector<Sample> &samples,
                                                           std::ptrdiff_t preamble_offset) const {
    if (preamble_offset < 0) {
        return std::nullopt;
    }

    const std::size_t needed = static_cast<std::size_t>(preamble_offset) + (kPreambleSymCount + kSyncSymCount) * sps_;
    if (samples.size() < needed) {
        return std::nullopt;
    }

    SyncWordDetection detection;
    detection.preamble_offset = static_cast<std::size_t>(preamble_offset);
    detection.symbol_bins.reserve(kPreambleSymCount + kSyncSymCount);
    detection.magnitudes.reserve(kPreambleSymCount + kSyncSymCount);

    FFTScratch scratch;
    scratch.input.reserve(sps_);
    scratch.spectrum.reserve(sps_);

    bool preamble_ok = true;

    for (std::size_t sym = 0; sym < kPreambleSymCount; ++sym) {
        double mag = 0.0;
        const std::size_t bin = demod_symbol(samples, sym, preamble_offset, scratch, mag);
        detection.symbol_bins.push_back(static_cast<int>(bin));
        detection.magnitudes.push_back(mag);
        if (bin != 0) {
            preamble_ok = false;
        }
    }

    detection.preamble_ok = preamble_ok;

    const unsigned nibble_hi = ((sync_word_ >> 4U) & 0x0FU) << 3U;
    const unsigned nibble_lo = (sync_word_ & 0x0FU) << 3U;
    const unsigned expected_sync[] = {nibble_hi, nibble_lo};

    bool sync_ok = true;
    for (std::size_t idx = 0; idx < kSyncSymCount; ++idx) {
        double mag = 0.0;
        const std::size_t sym_index = kPreambleSymCount + idx;
        const std::size_t bin = demod_symbol(samples, sym_index, preamble_offset, scratch, mag);
        detection.symbol_bins.push_back(static_cast<int>(bin));
        detection.magnitudes.push_back(mag);
        if (bin != expected_sync[idx]) {
            sync_ok = false;
        }
    }

    detection.sync_ok = sync_ok;
    return detection;
}

} // namespace lora
