#include "lora/rx/frame.hpp"
#include "lora/rx/header_decode.hpp"
#include "lora/rx/payload_decode.hpp"
#include "lora/utils/gray.hpp"
#include "lora/utils/crc.hpp"
#include "lora/utils/whitening.hpp"
#include "lora/rx/preamble.hpp"
#include "lora/debug.hpp"
#include "lora/rx/decimate.hpp"
#include "lora/rx/sync.hpp"
#include "lora/rx/demod.hpp"
#include <algorithm>

namespace lora::rx {

// Auto-length from header: OS-aware detect/align, then decode header to get payload length,
// then decode payload+CRC and verify.
std::pair<std::span<uint8_t>, bool> decode_frame_with_preamble_cfo_sto_os_auto(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    
    if (std::getenv("LORA_DEBUG")) {
        printf("DEBUG: decode_frame_with_preamble_cfo_sto_os_auto called\n");
    }
    bool __dbg = std::getenv("LORA_DEBUG");
    
    auto align_res = align_to_sync(ws, samples, sf, min_preamble_syms, expected_sync);
    if (!align_res) return {std::span<uint8_t>{}, false};
    auto aligned_vec = std::move(align_res->first);
    size_t sync_start = align_res->second;
    auto aligned = std::span<const std::complex<float>>(aligned_vec);
    // Heuristic: if a second sync symbol follows, skip it; then advance by two downchirps + quarter (2.25 symbols)
    {
        ws.init(sf);
        uint32_t N = ws.N;
        // Second sync check
        if (sync_start + N + N <= aligned.size()) {
            uint32_t ss2 = demod_symbol_peak(ws, &aligned[sync_start + N]);
            if (ss2 == expected_sync) {
                sync_start += N;
            }
        }
        auto corr_mag = [&](size_t idx, const std::complex<float>* ref) -> float {
            std::complex<float> acc(0.f,0.f);
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
        // Regardless of correlation strength, header begins after the two downchirps + quarter
        sync_start += (2u * N + N/4u);
        if (__dbg) printf("DEBUG: Advancing to header start at sync+2.25 symbols (sync_start=%zu)\n", sync_start);
    }
    uint32_t N = ws.N;
    // Data starts exactly at computed header start (after 2.25 symbols from sync)
    auto data = std::span<const std::complex<float>>(aligned.data() + sync_start,
                                                     aligned.size() - sync_start);

    if (__dbg) printf("DEBUG: Signal info - aligned.size()=%zu, sync_start=%zu, N=%u\n", aligned.size(), sync_start, N);
    if (__dbg) printf("DEBUG: Data span - offset=%zu, data.size()=%zu\n", sync_start + 3*N, data.size());
    
    const uint32_t header_cr_plus4 = 8u;
    const size_t hdr_bytes = 5;
    const size_t hdr_bits_exact = hdr_bytes * 2 * header_cr_plus4;

    auto header_res = decode_header_from_symbols(ws, data, sf);
    auto hdr_opt = header_res.first;
    auto hdr_nibbles = std::move(header_res.second);
    if (__dbg) {
        printf("DEBUG: Decoded header nibbles: ");
        for (auto n : hdr_nibbles) printf("0x%x ", n);
        printf("\n");
    }
    if (!hdr_opt) {
        lora::debug::set_fail(109);
        return {std::span<uint8_t>{}, false};
    }

    const size_t payload_len = hdr_opt->payload_len;
    // Record payload params for instrumentation
    ws.dbg_payload_len = static_cast<uint32_t>(payload_len);
    ws.dbg_cr_payload = hdr_opt->cr;

    printf("DEBUG: Payload processing - payload_len=%zu\n", payload_len);
    printf("DEBUG: Using header CR=%d for payload instead of input CR=%d\n",
           static_cast<int>(hdr_opt->cr), static_cast<int>(cr));
    printf("DEBUG: Header bits used: %zu\n", hdr_bits_exact);

    // Compute header symbol count (padded to interleaver block)
    size_t hdr_bits_padded = hdr_bits_exact;
    const uint32_t header_block_bits = sf * header_cr_plus4;
    if (hdr_bits_padded % header_block_bits) hdr_bits_padded = ((hdr_bits_padded / header_block_bits) + 1) * header_block_bits;
    const size_t hdr_nsym_pad = hdr_bits_padded / sf;

    auto payload_span = data.subspan(hdr_nsym_pad * ws.N);
    auto payload_res = decode_payload_with_crc(ws, payload_span, sf, *hdr_opt);
    if (!payload_res.second) {
        return {std::span<uint8_t>{}, false};
    }
    ws.rx_data = std::move(payload_res.first);
    return {std::span<uint8_t>(ws.rx_data.data(), ws.rx_data.size()), true};
}

std::optional<LocalHeader> decode_header_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    if (std::getenv("LORA_DEBUG"))
        printf("DEBUG: decode_header_with_preamble_cfo_sto_os called!\n");

    return decode_header_with_preamble_cfo_sto_os_impl(ws, samples, sf, cr,
                                                       min_preamble_syms,
                                                       expected_sync);
}

std::pair<std::vector<uint8_t>, bool> decode_payload_no_crc_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    return decode_payload_no_crc_with_preamble_cfo_sto_os_impl(ws, samples, sf, cr, min_preamble_syms, expected_sync);
}
} // namespace lora::rx
