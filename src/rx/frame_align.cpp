#include "lora/rx/frame_align.hpp"
#include "lora/rx/sync.hpp"
#include "lora/rx/demod.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace lora::rx {

std::optional<std::pair<std::vector<std::complex<float>>, size_t>>
align_and_extract_data(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    bool __dbg = std::getenv("LORA_DEBUG");

    auto align_res = align_to_sync(ws, samples, sf, min_preamble_syms, expected_sync);
    if (!align_res) return std::nullopt;
    auto aligned_vec = std::move(align_res->first);
    size_t sync_start = align_res->second;
    auto aligned = std::span<const std::complex<float>>(aligned_vec);

    {
        ws.init(sf);
        uint32_t N = ws.N;
        if (sync_start + N + N <= aligned.size()) {
            uint32_t ss2 = demod_symbol_peak(ws, &aligned[sync_start + N]);
            if (ss2 == expected_sync) {
                sync_start += N;
            }
        }
        auto corr_mag = [&](size_t idx, const std::complex<float>* ref) -> float {
            std::complex<float> acc(0.f, 0.f);
            if (idx + N > aligned.size()) return 0.f;
            for (uint32_t n = 0; n < N; ++n) acc += aligned[idx + n] * std::conj(ref[n]);
            return std::abs(acc);
        };
        size_t s1 = sync_start + N;
        size_t s2 = s1 + N;
        float up1 = corr_mag(s1, ws.upchirp.data());
        float dn1 = corr_mag(s1, ws.downchirp.data());
        float up2 = corr_mag(s2, ws.upchirp.data());
        float dn2 = corr_mag(s2, ws.downchirp.data());
        const float ratio = 2.0f;
        (void)up1; (void)dn1; (void)up2; (void)dn2; (void)ratio;
        sync_start += (2u * N + N/4u);
        if (__dbg) printf("DEBUG: Advancing to header start at sync+2.25 symbols (sync_start=%zu)\n", sync_start);
    }

    return std::pair<std::vector<std::complex<float>>, size_t>{std::move(aligned_vec), sync_start};
}

} // namespace lora::rx
