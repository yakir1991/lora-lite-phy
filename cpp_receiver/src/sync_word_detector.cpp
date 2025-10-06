#include "sync_word_detector.hpp"

#include "chirp_generator.hpp"
#include "fft_utils.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>

// After the preamble and before the header, LoRa transmits two sync symbols that
// encode an 8-bit value. Verifying those symbols serves both as a false-positive
// guard (preamble tones can appear in noise) and as a quick sanity check of CFO
// compensation. This file packages that logic into `SyncWordDetector`, including
// a helper to demodulate individual symbols with CFO rotation and chirp removal.

namespace lora {

namespace {

constexpr std::size_t kPreambleSymCount = 8;
constexpr std::size_t kSyncSymCount = 2;

} // namespace

// SyncWordDetector validates the LoRa sync word immediately following the preamble.
// Given:
//  - Spreading factor (SF), bandwidth (Hz), and sample rate (Hz)
//  - The configured 8-bit sync word (two 4-bit nibbles)
//  - A preamble_offset (start of the preamble) and CFO estimate (Hz)
// It demodulates 8 preamble symbols and the 2 sync symbols using a downchirp
// dechirp + K-point IDFT-like peak search, compensates CFO, and checks that the
// two sync symbols match the expected bins encoded by the sync word nibbles.
//
// Output is a SyncWordDetection struct containing:
//  - preamble_ok: whether all preamble symbols normalize near DC (within tol)
//  - sync_ok: whether both sync symbols match the expected bins (within tol)
//  - symbol_bins: normalized preamble bins plus the two sync bins
//  - magnitudes: peak magnitudes per symbol (debug/quality metric)
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

// Demodulate one LoRa symbol starting at sym_index relative to preamble_offset.
// Steps:
//  1) CFO rotation compensation.
//  2) Multiply by a reference downchirp (dechirp).
//  3) Decimate to K chips (K=2^SF) by picking every os_factor-th sample.
//  4) Compute a K-point IDFT-like sum to estimate the tone bin.
//  5) Return the aligned bin index and its magnitude.
// Note: We align by subtracting 1 in K-domain (consistent with payload demod).
std::size_t SyncWordDetector::demod_symbol(const std::vector<Sample> &samples,
                                           std::size_t sym_index,
                                           std::ptrdiff_t preamble_offset,
                                           double cfo_hz,
                                           FFTScratch &scratch,
                                           double &magnitude) const {
    const std::ptrdiff_t start_signed = preamble_offset + static_cast<std::ptrdiff_t>(sym_index * sps_);
    if (start_signed < 0) {
        throw std::out_of_range("SyncWordDetector: negative start index");
    }
    const std::size_t start = static_cast<std::size_t>(start_signed);
    scratch.input.assign(sps_, std::complex<double>{});

    const double fs = static_cast<double>(sample_rate_hz_);
    const double Ts = 1.0 / fs;
    for (std::size_t i = 0; i < sps_; ++i) {
        const auto &s = samples[start + i];
        const double angle = -2.0 * std::numbers::pi * cfo_hz * Ts * static_cast<double>(i);
        const std::complex<double> rot(std::cos(angle), std::sin(angle));
        scratch.input[i] = std::complex<double>(s.real(), s.imag()) * downchirp_[i] * rot;
    }

    // Aggregate to K chips by summing all os_factor samples within each chip window,
    // then perform a K-point IDFT-like search. This preserves amplitude proportional
    // to samples-per-symbol (sps) rather than picking a single sample per chip.
    const std::size_t chips = static_cast<std::size_t>(1) << static_cast<std::size_t>(sf_);
    const std::size_t os_factor = sps_ / chips;
    std::vector<std::complex<double>> dec(chips, std::complex<double>{});
    for (std::size_t chip = 0; chip < chips; ++chip) {
        const std::size_t base = chip * os_factor;
        std::complex<double> acc{0.0, 0.0};
        for (std::size_t j = 0; j < os_factor; ++j) {
            acc += scratch.input[base + j];
        }
        dec[chip] = acc;
    }

    std::vector<std::complex<double>> spec = dec;
    lora::fft::transform_pow2(spec, /*inverse=*/true);

    std::size_t best_k = 0;
    double best_mag = 0.0;
    for (std::size_t k = 0; k < spec.size(); ++k) {
        const double mag = std::abs(spec[k]);
        if (mag > best_mag) {
            best_mag = mag;
            best_k = k;
        }
    }

    // Align with payload: pos-1 in K-domain
    const std::size_t k_aligned = (best_k + chips - 1) % chips;
    // Since we sum os_factor samples per chip, best_mag is already on the order of sps.
    magnitude = best_mag;
    return k_aligned; // 0..K-1
}

std::optional<SyncWordDetection> SyncWordDetector::analyze(const std::vector<Sample> &samples,
                                                           std::ptrdiff_t preamble_offset,
                                                           double cfo_hz) const {
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

    const std::size_t chips_per_symbol = static_cast<std::size_t>(1) << static_cast<std::size_t>(sf_);
    const std::size_t tol = 2; // tolerance in bins (K-domain circular distance)

    // First pass: collect raw preamble bins
    std::vector<std::size_t> pre_bins;
    pre_bins.reserve(kPreambleSymCount);
    for (std::size_t sym = 0; sym < kPreambleSymCount; ++sym) {
        double mag = 0.0;
        const std::size_t bin = demod_symbol(samples, sym, preamble_offset, cfo_hz, scratch, mag);
        pre_bins.push_back(bin);
        detection.magnitudes.push_back(mag);
    }

    // Estimate constant offset in K-domain from preamble by taking the mode of bins.
    // This compensates for any constant rotation/offset so that preamble bins map near 0.
    std::size_t offset_est = 0;
    {
        std::size_t best_count = 0;
        for (std::size_t i = 0; i < pre_bins.size(); ++i) {
            std::size_t val = pre_bins[i];
            std::size_t cnt = 0;
            for (std::size_t j = 0; j < pre_bins.size(); ++j) {
                if (pre_bins[j] == val) cnt++;
            }
            if (cnt > best_count) {
                best_count = cnt;
                offset_est = val;
            }
        }
    }

    // Normalize preamble bins by removing the offset_est and check proximity to DC (0),
    // allowing circular distance within 'tol' bins. If any exceeds, preamble_ok=false.
    bool preamble_ok = true;
    for (std::size_t i = 0; i < pre_bins.size(); ++i) {
        std::size_t norm = (pre_bins[i] + chips_per_symbol - offset_est) % chips_per_symbol;
        detection.symbol_bins.push_back(static_cast<int>(norm));
        const std::size_t dist0 = std::min(norm, chips_per_symbol - norm);
        if (dist0 > tol) preamble_ok = false;
    }
    detection.preamble_ok = preamble_ok;

    const std::size_t nibble_hi = ((sync_word_ >> 4U) & 0x0FU) << 3U;
    const std::size_t nibble_lo = (sync_word_ & 0x0FU) << 3U;
    const std::size_t expected_sync_sym[] = {nibble_hi, nibble_lo};

    // Validate the two sync symbols. We normalize by offset_est and then choose
    // the orientation (bin or its complement) that minimizes circular distance
    // to the expected bin for this nibble. If distance > tol, mark sync_ok=false.
    bool sync_ok = true;
    for (std::size_t idx = 0; idx < kSyncSymCount; ++idx) {
        double mag = 0.0;
        const std::size_t sym_index = kPreambleSymCount + idx;
        const std::size_t raw_bin = demod_symbol(samples, sym_index, preamble_offset, cfo_hz, scratch, mag);
        std::size_t bin = (raw_bin + chips_per_symbol - offset_est) % chips_per_symbol;
        const std::size_t exp_bin = ((idx == 0) ? nibble_hi : nibble_lo) % chips_per_symbol;
        // Choose orientation (bin or its complement) that is closest to expected
        const std::size_t comp = (chips_per_symbol - bin) % chips_per_symbol;
        auto dist_circ = [chips_per_symbol](std::size_t a, std::size_t b){
            std::size_t d = (a > b) ? (a - b) : (b - a);
            return std::min(d, chips_per_symbol - d);
        };
        const std::size_t d_bin = dist_circ(bin, exp_bin);
        const std::size_t d_comp = dist_circ(comp, exp_bin);
        if (d_comp < d_bin) bin = comp;
        detection.symbol_bins.push_back(static_cast<int>(bin));
        detection.magnitudes.push_back(mag);
        std::size_t dist = dist_circ(bin, exp_bin);
        if (dist > tol) sync_ok = false;
    }

    detection.sync_ok = sync_ok;
    return detection;
}

} // namespace lora
