#include "lora/rx/gr/primitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <vector>

#if __has_include(<liquid/liquid.h>)
#include <liquid/liquid.h>
#elif __has_include(<liquid.h>)
#include <liquid.h>
#else
#error "liquid-dsp headers not found; initialise external/liquid-dsp or install the library."
#endif

namespace lora::rx::gr {

namespace {

uint32_t demod_symbol_internal(Workspace& ws, const std::complex<float>* block, bool apply_preamble_shift) {
    uint32_t N = ws.N;
    for (uint32_t n = 0; n < N; ++n)
        ws.rxbuf[n] = block[n] * ws.downchirp[n];
    ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
    uint32_t max_bin = 0;
    float max_mag = 0.f;
    for (uint32_t k = 0; k < N; ++k) {
        float mag = std::norm(ws.fftbuf[k]);
        if (mag > max_mag) {
            max_mag = mag;
            max_bin = k;
        }
    }
    if (apply_preamble_shift)
        max_bin = (max_bin + N - 44u) & (N - 1u);
    return max_bin;
}

std::optional<size_t> detect_preamble_corr(Workspace& ws,
                                            std::span<const std::complex<float>> samples,
                                            uint32_t sf,
                                            size_t min_syms) {
    ws.init(sf);
    uint32_t N = ws.N;
    size_t nsamp = samples.size();
    if (nsamp < N) return std::nullopt;

    std::vector<std::complex<float>> conj_up(N);
    for (uint32_t n = 0; n < N; ++n)
        conj_up[n] = std::conj(ws.upchirp[n]);

    size_t npos = nsamp - N + 1;
    std::vector<float> mags(npos, 0.f);
    float max_mag = 0.f;
    for (size_t i = 0; i < npos; ++i) {
        const auto* blk = &samples[i];
        std::complex<float> acc{0.f, 0.f};
        for (uint32_t n = 0; n < N; ++n)
            acc += blk[n] * conj_up[n];
        float mag = std::abs(acc);
        mags[i] = mag;
        if (mag > max_mag) max_mag = mag;
    }
    if (max_mag == 0.f) return std::nullopt;

    float threshold = max_mag * 0.4f;
    if (const char* dbg = std::getenv("LORA_DEBUG")) {
        (void)dbg;
        std::fprintf(stderr, "[preamble-corr] nsamp=%zu max=%.6f thr=%.6f\n", nsamp, (double)max_mag, (double)threshold);
        size_t preview = std::min<size_t>(16, mags.size());
        for (size_t i = 0; i < preview; ++i)
            std::fprintf(stderr, "  mag[%zu]=%.6f\n", i, (double)mags[i]);
    }

    size_t best_start = 0;
    bool found = false;
    for (size_t start = 0; start + min_syms * N <= nsamp; ++start) {
        bool ok = true;
        for (size_t k = 0; k < min_syms; ++k) {
            size_t idx = start + k * N;
            if (idx >= mags.size() || mags[idx] < threshold) {
                ok = false;
                break;
            }
        }
        if (ok) {
            best_start = start;
            found = true;
            break;
        }
    }

    if (!found) {
        auto it = std::max_element(mags.begin(), mags.end());
        best_start = static_cast<size_t>(std::distance(mags.begin(), it));
        found = true;
    }

    return found ? std::optional<size_t>(best_start) : std::nullopt;
}

} // namespace

std::vector<std::complex<float>> decimate_os_phase(std::span<const std::complex<float>> x,
                                                   int os,
                                                   int phase,
                                                   float attenuation_db) {
    if (os <= 1) {
        size_t start = (phase > 0 && static_cast<size_t>(phase) < x.size()) ? static_cast<size_t>(phase) : 0u;
        return std::vector<std::complex<float>>(x.begin() + start, x.end());
    }
    if (phase < 0 || phase >= os) phase = ((phase % os) + os) % os;

    const unsigned int M = static_cast<unsigned int>(os);
    const float fc = 0.45f / static_cast<float>(os);
    const unsigned int L = std::max<unsigned int>(32u * M, 8u * M);
    std::vector<float> taps(L);
    liquid_firdes_kaiser(L, fc, attenuation_db, 0.0f, taps.data());
    firdecim_crcf q = firdecim_crcf_create(M, taps.data(), L);
    if (!q) throw std::runtime_error("firdecim_crcf_create failed");

    if (static_cast<size_t>(phase) >= x.size()) {
        firdecim_crcf_destroy(q);
        return {};
    }
    const std::complex<float>* in = x.data() + static_cast<size_t>(phase);
    size_t usable = ((x.size() - static_cast<size_t>(phase)) / static_cast<size_t>(os)) * static_cast<size_t>(os);
    size_t n_out = usable / static_cast<size_t>(os);
    std::vector<std::complex<float>> out(n_out);
    size_t yo = 0;
    for (size_t i = 0; i < usable; i += static_cast<size_t>(os)) {
        liquid_float_complex acc;
        firdecim_crcf_execute(q,
                              reinterpret_cast<liquid_float_complex*>(const_cast<std::complex<float>*>(in + i)),
                              &acc);
        out[yo++] = std::complex<float>(acc);
    }
    firdecim_crcf_destroy(q);
    return out;
}

uint32_t demod_symbol_peak(Workspace& ws, const std::complex<float>* block) {
    return demod_symbol_internal(ws, block, false);
}

std::optional<size_t> detect_preamble(Workspace& ws,
                                      std::span<const std::complex<float>> samples,
                                      uint32_t sf,
                                      size_t min_syms) {
    if (min_syms == 0) return std::nullopt;
    return detect_preamble_corr(ws, samples, sf, min_syms);
}

std::optional<float> estimate_cfo_from_preamble(Workspace& ws,
                                                std::span<const std::complex<float>> samples,
                                                uint32_t sf,
                                                size_t start_sample,
                                                size_t preamble_syms) {
    ws.init(sf);
    uint32_t N = ws.N;
    if (start_sample + preamble_syms * N > samples.size())
        return std::nullopt;

    std::complex<float> acc{0.f, 0.f};
    for (size_t s = 0; s < preamble_syms; ++s) {
        const auto* block = &samples[start_sample + s * N];
        for (uint32_t n = 0; n < N; ++n)
            ws.rxbuf[n] = block[n] * ws.downchirp[n];
        for (uint32_t n = 1; n < N; ++n)
            acc += ws.rxbuf[n] * std::conj(ws.rxbuf[n - 1]);
    }
    if (std::abs(acc) == 0.f) return std::nullopt;
    float phi = std::arg(acc);
    float eps = phi / (2.0f * static_cast<float>(M_PI));
    return eps;
}

std::optional<int> estimate_sto_from_preamble(Workspace& ws,
                                              std::span<const std::complex<float>> samples,
                                              uint32_t sf,
                                              size_t start_sample,
                                              size_t preamble_syms,
                                              int search_radius) {
    ws.init(sf);
    uint32_t N = ws.N;
    if (start_sample + preamble_syms * N > samples.size())
        return std::nullopt;

    size_t base = start_sample;
    std::vector<std::complex<float>> conj_up(N);
    for (uint32_t n = 0; n < N; ++n)
        conj_up[n] = std::conj(ws.upchirp[n]);

    float best_mag = -1.f;
    int best_shift = 0;
    for (int s = -search_radius; s <= search_radius; ++s) {
        if (s < 0 && static_cast<size_t>(-s) > base) continue;
        size_t shifted = (s >= 0) ? base + static_cast<size_t>(s) : base - static_cast<size_t>(-s);
        if (shifted + N > samples.size()) break;
        std::complex<float> corr{0.f, 0.f};
        const auto* blk = &samples[shifted];
        for (uint32_t n = 0; n < N; ++n)
            corr += blk[n] * conj_up[n];
        float mag = std::abs(corr);
        if (mag > best_mag) {
            best_mag = mag;
            best_shift = s;
        }
    }
    return best_shift;
}

std::optional<PreambleDetectResult> detect_preamble_os(Workspace& ws,
                                                       std::span<const std::complex<float>> samples,
                                                       uint32_t sf,
                                                       size_t min_syms,
                                                       const std::vector<int>& os_candidates) {
    std::vector<int> candidates = os_candidates.empty() ? std::vector<int>{1, 2, 4, 8} : os_candidates;
    for (int os : candidates) {
        if (os <= 0) continue;
        int phase_count = std::max(os, 1);
        for (int phase = 0; phase < phase_count; ++phase) {
            auto decim = decimate_os_phase(samples, os, phase);
            auto pos = detect_preamble_corr(ws, decim, sf, min_syms);
            if (!pos) continue;
            size_t start_raw = (*pos) * static_cast<size_t>(os) + static_cast<size_t>(phase);
            unsigned int L = static_cast<unsigned int>(std::max(32 * os, 8 * os));
            size_t gd_raw = static_cast<size_t>(L / 2);
            size_t adj_raw = start_raw > gd_raw ? (start_raw - gd_raw) : 0u;
            return PreambleDetectResult{adj_raw, os, phase};
        }
    }
    return std::nullopt;
}

std::optional<PreambleDetectResult> detect_preamble_os(Workspace& ws,
                                                       std::span<const std::complex<float>> samples,
                                                       uint32_t sf,
                                                       size_t min_syms,
                                                       std::initializer_list<int> os_candidates) {
    std::vector<int> vec(os_candidates.begin(), os_candidates.end());
    return detect_preamble_os(ws, samples, sf, min_syms, vec);
}

} // namespace lora::rx::gr
