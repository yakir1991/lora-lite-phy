#include "lora/rx/frame.hpp"
#include "lora/rx/frame_align.hpp"
#include "lora/rx/header_decode.hpp"
#include "lora/rx/payload_decode.hpp"
#include "lora/debug.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace lora::rx {

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

    auto align_res = align_and_extract_data(ws, samples, sf, min_preamble_syms, expected_sync);
    if (!align_res) return {std::span<uint8_t>{}, false};
    auto aligned_vec = std::move(align_res->first);
    size_t hdr_start_base = align_res->second;
    auto aligned = std::span<const std::complex<float>>(aligned_vec);
    auto data = aligned.subspan(hdr_start_base);

    uint32_t N = ws.N;

    if (__dbg) {
        printf("DEBUG: Signal info - aligned.size()=%zu, hdr_start_base=%zu, N=%u\n", aligned.size(), hdr_start_base, N);
        printf("DEBUG: Data span - offset=%zu, data.size()=%zu\n", hdr_start_base + 3*N, data.size());
    }

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
    ws.dbg_payload_len = static_cast<uint32_t>(payload_len);
    ws.dbg_cr_payload = hdr_opt->cr;

    printf("DEBUG: Payload processing - payload_len=%zu\n", payload_len);
    printf("DEBUG: Using header CR=%d for payload instead of input CR=%d\n",
           static_cast<int>(hdr_opt->cr), static_cast<int>(cr));
    printf("DEBUG: Header bits used: %zu\n", hdr_bits_exact);

    size_t hdr_bits_padded = hdr_bits_exact;
    const uint32_t header_block_bits = sf * header_cr_plus4;
    if (hdr_bits_padded % header_block_bits)
        hdr_bits_padded = ((hdr_bits_padded / header_block_bits) + 1) * header_block_bits;
    const size_t hdr_nsym_pad = hdr_bits_padded / sf;

    auto payload_span = data.subspan(hdr_nsym_pad * ws.N);
    auto payload_res = decode_payload_with_crc(ws, payload_span, sf, *hdr_opt);
    if (!payload_res.second) {
        return {std::span<uint8_t>{}, false};
    }
    ws.rx_data = std::move(payload_res.first);
    return {std::span<uint8_t>(ws.rx_data.data(), ws.rx_data.size()), true};
}

} // namespace lora::rx
