#include "lora/rx/sync.hpp"
#include "lora/rx/decimate.hpp"
#include "lora/rx/preamble.hpp"
#include "lora/debug.hpp"
#include <cmath>

namespace lora::rx {

std::optional<std::pair<std::vector<std::complex<float>>, size_t>> align_to_sync(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    // Detect OS and phase
    auto det = detect_preamble_os(ws, samples, sf, min_preamble_syms, {4,2,1,8});
    if (!det) return std::nullopt;
    // Decimate to OS=1
    auto decim = decimate_os_phase(samples, det->os, det->phase);
    size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
    if (start_decim >= decim.size()) return std::nullopt;
    auto aligned0 = std::span<const std::complex<float>>(decim.data() + start_decim,
                                                         decim.size() - start_decim);
    // Estimate CFO over preamble (OS=1 now)
    auto pos0 = detect_preamble(ws, aligned0, sf, min_preamble_syms);
    if (!pos0) { lora::debug::set_fail(102); return std::nullopt; }
    auto cfo_opt = estimate_cfo_from_preamble(ws, aligned0, sf, *pos0, min_preamble_syms);
    // Fallback: if CFO estimation fails, assume 0 and continue
    float cfo_val = cfo_opt.has_value() ? *cfo_opt : 0.0f;
    if (!cfo_opt) {
        lora::debug::set_fail(103);
    }
    // Compensate CFO
    std::vector<std::complex<float>> comp(aligned0.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (cfo_val);
    std::complex<float> j(0.f, 1.f);
    for (size_t n = 0; n < aligned0.size(); ++n)
        comp[n] = aligned0[n] * std::exp(j * (two_pi_eps * static_cast<float>(n)));
    // Estimate integer STO
    auto sto_opt = estimate_sto_from_preamble(ws, comp, sf, *pos0, min_preamble_syms, static_cast<int>(ws.N/8));
    int shift = 0;
    if (sto_opt) {
        shift = *sto_opt;
    } else {
        lora::debug::set_fail(104);
    }
    size_t aligned_start = (shift >= 0) ? (*pos0 + static_cast<size_t>(shift))
                                        : (*pos0 - static_cast<size_t>(-shift));
    if (aligned_start >= comp.size()) { lora::debug::set_fail(105); return std::nullopt; }
    // Drop samples before aligned_start
    if (aligned_start > 0) comp.erase(comp.begin(), comp.begin() + aligned_start);
    auto aligned = std::span<const std::complex<float>>(comp);
    // Check sync word with small elastic search (±2 symbols and small sample shifts)
    ws.init(sf);
    uint32_t N = ws.N;
    size_t sync_start = 0;
    bool found_sync2 = false;
    int sym_shifts2[5] = {0, -1, 1, -2, 2};
    int samp_shifts2[5] = {0, -(int)N/32, (int)N/32, -(int)N/16, (int)N/16};
    for (int s : sym_shifts2) {
        size_t base = (s >= 0) ? ((min_preamble_syms + (size_t)s) * N)
                                : ((min_preamble_syms - (size_t)(-s)) * N);
        for (int so : samp_shifts2) {
            if (so >= 0) {
                if (base + (size_t)so + N > aligned.size()) continue;
                size_t idx = base + (size_t)so;
                uint32_t ss = demod_symbol(ws, &aligned[idx]);
                if (ss == expected_sync) { found_sync2 = true; sync_start = idx; break; }
            } else {
                size_t offs = (size_t)(-so);
                if (base < offs) continue;
                size_t idx = base - offs;
                if (idx + N > aligned.size()) continue;
                uint32_t ss = demod_symbol(ws, &aligned[idx]);
                if (ss == expected_sync) { found_sync2 = true; sync_start = idx; break; }
            }
        }
        if (found_sync2) break;
    }
    if (!found_sync2) {
        // Fallback: windowed correlation around expected sync region on aligned sequence
        std::vector<std::complex<float>> ref(N);
        for (uint32_t n = 0; n < N; ++n)
            ref[n] = std::conj(ws.upchirp[(n + expected_sync) % N]);
        long best_off = 0; float best_mag = -1.f;
        int range = (int)N/8; int step = std::max<int>(1, (int)N/64);
        size_t base = min_preamble_syms * N;
        for (int off = -range; off <= range; off += step) {
            if (off >= 0) {
                if (base + (size_t)off + N > aligned.size()) continue;
                size_t idx = base + (size_t)off;
                std::complex<float> acc(0.f,0.f);
                for (uint32_t n = 0; n < N; ++n) acc += aligned[idx + n] * ref[n];
                float mag = std::abs(acc);
                if (mag > best_mag) { best_mag = mag; best_off = off; }
            } else {
                size_t offs = (size_t)(-off);
                if (base < offs) continue;
                size_t idx = base - offs;
                if (idx + N > aligned.size()) continue;
                std::complex<float> acc(0.f,0.f);
                for (uint32_t n = 0; n < N; ++n) acc += aligned[idx + n] * ref[n];
                float mag = std::abs(acc);
                if (mag > best_mag) { best_mag = mag; best_off = off; }
            }
        }
        if (best_mag > 0.f) {
            sync_start = (best_off >= 0) ? (base + (size_t)best_off)
                                        : (base - (size_t)(-best_off));
            found_sync2 = true;
        }
        if (!found_sync2) { lora::debug::set_fail(107); return std::nullopt; }
    }
    return std::pair{std::move(comp), sync_start};
}

} // namespace lora::rx
