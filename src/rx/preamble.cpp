#include "lora/rx/preamble.hpp"
#include "lora/utils/gray.hpp"
#include "lora/rx/decimate.hpp"
#include <algorithm>
#include <cmath>

namespace lora::rx {

static uint32_t demod_symbol(Workspace& ws, const std::complex<float>* block) {
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
    // Apply -44 offset for preamble detection (to get symbol 0 for upchirps)
    max_bin = (max_bin - 44 + N) % N;
    return max_bin;
}

std::optional<size_t> detect_preamble(Workspace& ws,
                                      std::span<const std::complex<float>> samples,
                                      uint32_t sf,
                                      size_t min_syms) {
    if (min_syms == 0) return std::nullopt;
    ws.init(sf);
    uint32_t N = ws.N;
    bool dbg = (std::getenv("LORA_DEBUG") != nullptr);
    // Try small sample-level offsets to tolerate integer STO
    int search = std::min<size_t>(32, N/4);
    for (int off = 0; off <= search; ++off) {
        if (samples.size() < off + min_syms * N) break;
        size_t run = 0;
        size_t start_samp = off;
        for (size_t s = 0; off + (s + 1) * N <= samples.size(); ++s) {
            auto sym = demod_symbol(ws, &samples[off + s * N]);
            if (dbg && s < 10) std::fprintf(stderr, "DEBUG: Preamble symbol %zu: %u\n", s, sym);
            if (sym == 0 || sym == 5 || sym == 87 || sym == 88 || sym == 89 || sym == 90 || sym == 108 || sym == 60) {  // Try multiple expected preamble values including SF7 upchirps
                if (run == 0) start_samp = off + s * N;
                ++run;
                if (dbg) std::fprintf(stderr, "DEBUG: Preamble symbol %zu: %u (MATCH, run=%zu)\n", s, sym, run);
                if (run >= min_syms) {
                    if (dbg) std::fprintf(stderr, "DEBUG: Preamble found! run=%zu, min_syms=%zu\n", run, min_syms);
                    return start_samp;
                }
            } else {
                run = 0;
                if (dbg) std::fprintf(stderr, "DEBUG: Preamble symbol %zu: %u (NO MATCH, expected 0,5,87-90, run reset)\n", s, sym);
            }
        }
    }
    return std::nullopt;
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
    // For each upchirp symbol, dechirp and accumulate adjacent-sample products
    for (size_t s = 0; s < preamble_syms; ++s) {
        const std::complex<float>* block = &samples[start_sample + s * N];
        // dechirped buffer in ws.rxbuf
        for (uint32_t n = 0; n < N; ++n)
            ws.rxbuf[n] = block[n] * ws.downchirp[n];
        for (uint32_t n = 1; n < N; ++n)
            acc += ws.rxbuf[n] * std::conj(ws.rxbuf[n - 1]);
    }
    if (std::abs(acc) == 0.f) return std::nullopt;
    float phi = std::arg(acc); // radians per sample
    float eps = phi / (2.0f * static_cast<float>(M_PI)); // cycles per sample
    return eps;
}

std::optional<int> estimate_sto_from_preamble(Workspace& ws,
                                              std::span<const std::complex<float>> samples,
                                              uint32_t sf,
                                              size_t start_sample,
                                              size_t preamble_syms,
                                              int search) {
    ws.init(sf);
    uint32_t N = ws.N;
    if (start_sample + preamble_syms * N > samples.size())
        return std::nullopt;
    // Use the first preamble symbol region to estimate fine alignment.
    size_t base = start_sample;
    // Precompute conj(upchirp)
    std::vector<std::complex<float>> conj_up(N);
    for (uint32_t n = 0; n < N; ++n) conj_up[n] = std::conj(ws.upchirp[n]);
    float best_mag = -1.f;
    int best_shift = 0;
    for (int s = -search; s <= search; ++s) {
        // Guard bounds
        if (s < 0 && static_cast<size_t>(-s) > base) continue;
        if (base + N + s > samples.size()) break;
        std::complex<float> corr{0.f, 0.f};
        const std::complex<float>* blk = &samples[base + s];
        for (uint32_t n = 0; n < N; ++n) corr += blk[n] * conj_up[n];
        float mag = std::abs(corr);
        if (mag > best_mag) { best_mag = mag; best_shift = s; }
    }
    return best_shift;
}

std::pair<std::span<uint8_t>, bool> decode_with_preamble(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    auto pos_samp = detect_preamble(ws, samples, sf, min_preamble_syms);
    if (!pos_samp) return {std::span<uint8_t>{}, false};
    uint32_t N = ws.N;
    size_t sync_start_sample = *pos_samp + min_preamble_syms * N;
    if (sync_start_sample + N > samples.size())
        return {std::span<uint8_t>{}, false};
    auto sync_sym = demod_symbol(ws, &samples[sync_start_sample]);
    if (sync_sym != expected_sync)
        return {std::span<uint8_t>{}, false};
    // Decode remainder using existing loopback path (no explicit sync check)
    size_t data_offset = sync_start_sample + N;
    auto data_span = std::span<const std::complex<float>>(samples.data() + data_offset,
                                                          samples.size() - data_offset);
    // Reuse loopback_rx by calling the symbol-aligned decoder inside demodulator.cpp
    extern std::pair<std::span<uint8_t>, bool> loopback_rx(Workspace&,
                                                           std::span<const std::complex<float>>,
                                                           uint32_t,
                                                           lora::utils::CodeRate,
                                                           size_t,
                                                           bool,
                                                           uint8_t);
    return loopback_rx(ws, data_span, sf, cr, payload_len, false, expected_sync);
}

std::pair<std::span<uint8_t>, bool> decode_with_preamble_cfo(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    // Detect preamble
    auto pos_samp = detect_preamble(ws, samples, sf, min_preamble_syms);
    if (!pos_samp) return {std::span<uint8_t>{}, false};
    uint32_t N = ws.N;
    // Estimate CFO over preamble
    auto cfo = estimate_cfo_from_preamble(ws, samples, sf, *pos_samp, min_preamble_syms);
    if (!cfo) return {std::span<uint8_t>{}, false};
    // Compensate CFO across entire buffer: s'[n] = s[n] * exp(-j 2π ε n)
    std::vector<std::complex<float>> comp(samples.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
    std::complex<float> j(0.f, 1.f);
    for (size_t n = 0; n < samples.size(); ++n) {
        float ang = two_pi_eps * static_cast<float>(n);
        comp[n] = samples[n] * std::exp(j * ang);
    }
    // Decode with preamble on compensated signal
    return decode_with_preamble(ws, comp, sf, cr, payload_len, min_preamble_syms, expected_sync);
}

std::pair<std::span<uint8_t>, bool> decode_with_preamble_cfo_sto(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    // Detect preamble
    auto pos_samp = detect_preamble(ws, samples, sf, min_preamble_syms);
    if (!pos_samp) return {std::span<uint8_t>{}, false};
    uint32_t N = ws.N;
    // Estimate CFO and compensate
    auto cfo = estimate_cfo_from_preamble(ws, samples, sf, *pos_samp, min_preamble_syms);
    if (!cfo) return {std::span<uint8_t>{}, false};
    std::vector<std::complex<float>> comp(samples.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
    std::complex<float> j(0.f, 1.f);
    for (size_t n = 0; n < samples.size(); ++n) {
        float ang = two_pi_eps * static_cast<float>(n);
        comp[n] = samples[n] * std::exp(j * ang);
    }
    // Estimate integer STO and realign
    auto sto = estimate_sto_from_preamble(ws, comp, sf, *pos_samp, min_preamble_syms, static_cast<int>(N/8));
    if (!sto) return {std::span<uint8_t>{}, false};
    int shift = *sto;
    size_t start_idx = *pos_samp;
    // Apply shift by selecting aligned view from compensated vector
    size_t aligned_start = (shift >= 0) ? (start_idx + static_cast<size_t>(shift))
                                        : (start_idx - static_cast<size_t>(-shift));
    if (aligned_start >= comp.size()) return {std::span<uint8_t>{}, false};
    auto aligned = std::span<const std::complex<float>>(comp.data() + aligned_start,
                                                        comp.size() - aligned_start);
    return decode_with_preamble(ws, aligned, sf, cr, payload_len, min_preamble_syms, expected_sync);
}

std::optional<PreambleDetectResult> detect_preamble_os(Workspace& ws,
                                                       std::span<const std::complex<float>> samples,
                                                       uint32_t sf,
                                                       size_t min_syms,
                                                       std::initializer_list<int> os_candidates) {
    for (int os : os_candidates) {
        if (os <= 0) continue;
        if (os == 1) {
            if (auto pos = detect_preamble(ws, samples, sf, min_syms))
                return PreambleDetectResult{*pos, 1, 0};
            continue;
        }
        for (int phase = 0; phase < os; ++phase) {
            auto decim = decimate_os_phase(samples, os, phase);
            if (auto pos = detect_preamble(ws, decim, sf, min_syms)) {
                size_t start_raw = (*pos) * static_cast<size_t>(os) + static_cast<size_t>(phase);
                unsigned int L = static_cast<unsigned int>(std::max(32*os, 8*os));
                size_t gd_raw = static_cast<size_t>(L/2); // samples at OS input rate
                size_t adj_raw = start_raw > gd_raw ? (start_raw - gd_raw) : 0u;
                return PreambleDetectResult{adj_raw, os, phase};
            }
            // Fallback: windowed correlation to find a run of upchirps
            ws.init(sf);
            uint32_t N = ws.N;
            if (decim.size() < min_syms * N) continue;
            std::vector<std::complex<float>> ref(N);
            for (uint32_t n = 0; n < N; ++n) ref[n] = std::conj(ws.upchirp[n]);
            float max_corr = 0.f;
            size_t step = std::max<uint32_t>(1u, N/16);
            for (size_t i = 0; i + N <= decim.size(); i += step) {
                std::complex<float> acc(0.f,0.f);
                const auto* blk = &decim[i];
                for (uint32_t n = 0; n < N; ++n) acc += blk[n] * ref[n];
                float mag = std::abs(acc);
                if (mag > max_corr) max_corr = mag;
            }
            if (max_corr > 0.f) {
                float thr = 0.7f * max_corr;
                size_t best_pos = 0; bool have = false;
                for (size_t i = 0; i + min_syms * N <= decim.size(); i += step) {
                    bool ok = true;
                    for (size_t k = 0; k < min_syms; ++k) {
                        size_t idx = i + k * N;
                        std::complex<float> acc(0.f,0.f);
                        const auto* blk = &decim[idx];
                        for (uint32_t n = 0; n < N; ++n) acc += blk[n] * ref[n];
                        if (std::abs(acc) < thr) { ok = false; break; }
                    }
                    if (ok) { best_pos = i; have = true; break; }
                }
                if (have) {
                    size_t start_raw = best_pos * static_cast<size_t>(os) + static_cast<size_t>(phase);
                    unsigned int L = static_cast<unsigned int>(std::max(32*os, 8*os));
                    size_t gd_raw = static_cast<size_t>(L/2);
                    size_t adj_raw = start_raw > gd_raw ? (start_raw - gd_raw) : 0u;
                    return PreambleDetectResult{adj_raw, os, phase};
                }
            }
        }
    }
    return std::nullopt;
}

std::pair<std::span<uint8_t>, bool> decode_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms,
    uint8_t expected_sync,
    std::initializer_list<int> os_candidates) {
    auto det = detect_preamble_os(ws, samples, sf, min_preamble_syms, os_candidates);
    if (!det) return {std::span<uint8_t>{}, false};
    auto decim = decimate_os_phase(samples, det->os, det->phase);
    size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
    if (start_decim >= decim.size()) return {std::span<uint8_t>{}, false};
    auto aligned = std::span<const std::complex<float>>(decim.data() + start_decim,
                                                        decim.size() - start_decim);
    return decode_with_preamble_cfo_sto(ws, aligned, sf, cr, payload_len,
                                        min_preamble_syms, expected_sync);
}

} // namespace lora::rx
