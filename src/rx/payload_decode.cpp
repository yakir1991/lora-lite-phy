#include "lora/rx/payload_decode.hpp"
#include "lora/utils/gray.hpp"
#include "lora/rx/demod.hpp"
#include "lora/utils/whitening.hpp"
#include "lora/rx/preamble.hpp"
#include "lora/utils/crc.hpp"
#include "lora/rx/decimate.hpp"
#include <vector>

namespace lora::rx {

// Local demod helper (same as other TUs)
// use demod_symbol_peak from demod.hpp

std::pair<std::vector<uint8_t>, bool> decode_payload_no_crc_with_preamble_cfo_sto_os_impl(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    // Align and decode header first
    auto det = detect_preamble_os(ws, samples, sf, min_preamble_syms, {1,2,4,8});
    if (!det) return {{}, false};
    auto decim = decimate_os_phase(samples, det->os, det->phase);
    size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
    if (start_decim >= decim.size()) return {{}, false};
    auto aligned0 = std::span<const std::complex<float>>(decim.data() + start_decim,
                                                         decim.size() - start_decim);
    auto pos0 = detect_preamble(ws, aligned0, sf, min_preamble_syms);
    if (!pos0) return {{}, false};
    auto cfo = estimate_cfo_from_preamble(ws, aligned0, sf, *pos0, min_preamble_syms);
    if (!cfo) return {{}, false};
    std::vector<std::complex<float>> comp(aligned0.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
    std::complex<float> j(0.f, 1.f);
    for (size_t n = 0; n < aligned0.size(); ++n)
        comp[n] = aligned0[n] * std::exp(j * (two_pi_eps * static_cast<float>(n)));
    auto sto = estimate_sto_from_preamble(ws, comp, sf, *pos0, min_preamble_syms, static_cast<int>(ws.N/8));
    if (!sto) return {{}, false};
    int shift = *sto;
    size_t aligned_start = (shift >= 0) ? (*pos0 + static_cast<size_t>(shift))
                                        : (*pos0 - static_cast<size_t>(-shift));
    if (aligned_start >= comp.size()) return {{}, false};
    auto aligned = std::span<const std::complex<float>>(comp.data() + aligned_start,
                                                        comp.size() - aligned_start);
    ws.init(sf);
    uint32_t N = ws.N;
    size_t sync_start = min_preamble_syms * N;
    if (sync_start + N > aligned.size()) return {{}, false};
    uint32_t net1 = ((expected_sync & 0xF0u) >> 4) << 3;
    uint32_t net2 = (expected_sync & 0x0Fu) << 3;
    uint32_t sync_sym = demod_symbol_peak(ws, &aligned[sync_start]);
    if (!(std::abs(int(sync_sym) - int(net1)) <= 2 || std::abs(int(sync_sym) - int(net2)) <= 2)) return {{}, false};
    // 2.25 symbol advance like GR
    auto data = std::span<const std::complex<float>>(aligned.data() + sync_start + (2u * N + N/4u),
                                                     aligned.size() - (sync_start + (2u * N + N/4u)));
    // Decode header with local format (existing behavior retained)
    uint32_t cr_plus4 = static_cast<uint32_t>(cr) + 4;
    size_t hdr_bytes = 5;  // Standard LoRa header is 5 bytes
    size_t hdr_bits = hdr_bytes * 2 * cr_plus4;
    uint32_t block_bits = sf * cr_plus4;
    if (hdr_bits % block_bits) hdr_bits = ((hdr_bits / block_bits) + 1) * block_bits;
    size_t hdr_nsym = hdr_bits / sf;
    if (data.size() < hdr_nsym * N) return {{}, false};
    ws.ensure_rx_buffers(hdr_nsym, sf, cr_plus4);
    auto& symbols = ws.rx_symbols;
    for (size_t s = 0; s < hdr_nsym; ++s) {
        uint32_t raw_symbol = demod_symbol_peak(ws, &data[s * N]);
        symbols[s] = lora::utils::gray_encode(raw_symbol);
    }
    auto& bits = ws.rx_bits; auto& deint = ws.rx_deint; auto& nibbles = ws.rx_nibbles;
    size_t bit_idx = 0;
    for (size_t s = 0; s < hdr_nsym; ++s) {
        uint32_t sym = symbols[s];
        for (uint32_t b = 0; b < sf; ++b) bits[bit_idx++] = (sym >> b) & 1;
    }
    const auto& M = ws.get_interleaver(sf, cr_plus4);
    for (size_t off = 0; off < bit_idx; off += M.n_in)
        for (uint32_t i = 0; i < M.n_out; ++i) deint[off + M.map[i]] = bits[off + i];
    static lora::utils::HammingTables T = lora::utils::make_hamming_tables();
    size_t nib_idx = 0;
    for (size_t i = 0; i < hdr_bits; i += cr_plus4) {
        uint16_t cw = 0; for (uint32_t b = 0; b < cr_plus4; ++b) cw = (cw << 1) | deint[i + b];
        auto dec = lora::utils::hamming_decode4(cw, cr_plus4, cr, T);
        if (!dec) return {{}, false};
        nibbles[nib_idx++] = dec->first & 0x0F;
    }
    std::vector<uint8_t> hdr(hdr_bytes);
    for (size_t i = 0; i < hdr_bytes; ++i) {
        uint8_t low  = nibbles[i*2];
        uint8_t high = nibbles[i*2+1];
        hdr[i] = (high << 4) | low;
    }
    auto lfsr = lora::utils::LfsrWhitening::pn9_default();
    lfsr.apply(hdr.data(), hdr.size());
    auto hdr_opt = parse_local_header_with_crc(hdr.data(), hdr.size());
    if (!hdr_opt) return {{}, false};
    size_t payload_len = hdr_opt->payload_len;

    // Decode payload without enforcing CRC
    size_t pay_crc_bytes = payload_len + 2;
    size_t pay_bits = pay_crc_bytes * 2 * cr_plus4;
    if (pay_bits % block_bits) pay_bits = ((pay_bits / block_bits) + 1) * block_bits;
    size_t pay_nsym = pay_bits / sf;
    if (data.size() < (hdr_nsym + pay_nsym) * N) return {{}, true};
    ws.ensure_rx_buffers(pay_nsym, sf, cr_plus4);
    for (size_t s = 0; s < pay_nsym; ++s) {
        uint32_t raw_symbol = demod_symbol_peak(ws, &data[(hdr_nsym + s) * N]);
        symbols[s] = lora::utils::gray_encode(raw_symbol);
    }
    bit_idx = 0;
    for (size_t s = 0; s < pay_nsym; ++s) {
        uint32_t sym = symbols[s];
        for (uint32_t b = 0; b < sf; ++b) bits[bit_idx++] = (sym >> b) & 1;
    }
    for (size_t off = 0; off < bit_idx; off += M.n_in)
        for (uint32_t i = 0; i < M.n_out; ++i) deint[off + M.map[i]] = bits[off + i];
    nib_idx = 0;
    for (size_t i = 0; i < pay_bits; i += cr_plus4) {
        uint16_t cw = 0; for (uint32_t b = 0; b < cr_plus4; ++b) cw = (cw << 1) | deint[i + b];
        auto dec = lora::utils::hamming_decode4(cw, cr_plus4, cr, T);
        if (!dec) return {{}, true};
        nibbles[nib_idx++] = dec->first & 0x0F;
        if (nib_idx >= pay_crc_bytes * 2) break;
    }
    std::vector<uint8_t> out(pay_crc_bytes);
    for (size_t i = 0; i < pay_crc_bytes; ++i) {
        uint8_t low  = (i*2   < nib_idx) ? nibbles[i*2]     : 0u;
        uint8_t high = (i*2+1 < nib_idx) ? nibbles[i*2 + 1] : 0u;
        out[i] = (uint8_t)((high << 4) | low);
    }
    // Dewhiten payload only (exclude CRC)
    if (payload_len > 0) {
        auto lfsr2 = lora::utils::LfsrWhitening::pn9_default();
        lfsr2.apply(out.data(), payload_len);
    }
    return {out, true};
}

} // namespace lora::rx
