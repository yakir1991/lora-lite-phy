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
    const size_t   total_syms = data.size() / ws.N;
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
    const size_t pay_crc_bytes = payload_len + 2;
    
    // Use the coding rate from the header for payload decoding
    uint32_t payload_cr_plus4 = static_cast<uint32_t>(hdr_opt->cr) + 4;
    const size_t pay_bits_exact = pay_crc_bytes * 2 * payload_cr_plus4;
    
    printf("DEBUG: Payload processing - payload_len=%zu, pay_crc_bytes=%zu, pay_bits_exact=%zu\n", 
           payload_len, pay_crc_bytes, pay_bits_exact);
    printf("DEBUG: Using header CR=%d (cr_plus4=%u) for payload instead of input CR=%d\n", 
           static_cast<int>(hdr_opt->cr), payload_cr_plus4, static_cast<int>(cr));
    printf("DEBUG: Header bits used: %zu\n", hdr_bits_exact);

    // Reconstruct payload bitstream using payload CR interleaver (do not reuse header-mapped stream_bits)
    // Compute header symbol count (padded to interleaver block)
    size_t hdr_bits_padded = hdr_bits_exact;
    const uint32_t header_block_bits = sf * header_cr_plus4;
    if (hdr_bits_padded % header_block_bits) hdr_bits_padded = ((hdr_bits_padded / header_block_bits) + 1) * header_block_bits;
    const size_t hdr_nsym_pad = hdr_bits_padded / sf;
    // Compute payload symbol count (padded to payload interleaver block)
    size_t pay_bits_padded = pay_bits_exact;
    const uint32_t payload_block_bits = sf * payload_cr_plus4;
    if (pay_bits_padded % payload_block_bits) pay_bits_padded = ((pay_bits_padded / payload_block_bits) + 1) * payload_block_bits;
    const size_t pay_nsym = pay_bits_padded / sf;
    // Bounds check using existing total_syms computed earlier
    if (hdr_nsym_pad + pay_nsym > total_syms) {
        lora::debug::set_fail(110);
        return {std::span<uint8_t>{}, false};
    }
    // Demod payload symbols and build MSB-first bits
    ws.ensure_rx_buffers(pay_nsym, sf, payload_cr_plus4);
    auto& symbols_pay = ws.rx_symbols; symbols_pay.resize(pay_nsym);
    for (size_t s = 0; s < pay_nsym; ++s) {
        uint32_t raw_symbol = demod_symbol_peak(ws, &data[(hdr_nsym_pad + s) * ws.N]);
        symbols_pay[s] = lora::utils::gray_encode(raw_symbol);
    }
    auto& bits_pay = ws.rx_bits; bits_pay.resize(pay_nsym * sf);
    size_t bix = 0;
    for (size_t s = 0; s < pay_nsym; ++s) {
        uint32_t sym = symbols_pay[s];
        for (int b = (int)sf - 1; b >= 0; --b) bits_pay[bix++] = (sym >> b) & 1u;
    }
    // Deinterleave with payload CR mapping
    const auto& Mp = ws.get_interleaver(sf, payload_cr_plus4);
    auto& deint_pay = ws.rx_deint; deint_pay.resize(bix);
    for (size_t off = 0; off + Mp.n_in <= bix; off += Mp.n_in)
        for (uint32_t i = 0; i < Mp.n_out; ++i)
            deint_pay[off + Mp.map[i]] = bits_pay[off + i];
    // Hamming decode to nibbles
    auto& nibbles = ws.rx_nibbles;
    nibbles.resize(pay_crc_bytes * 2);
    size_t nib_idx = 0;
    static lora::utils::HammingTables T = lora::utils::make_hamming_tables();
    bool fec_failed = false;
    for (size_t i = 0; i < pay_bits_exact; i += payload_cr_plus4) {
        uint16_t cw = 0; for (uint32_t b = 0; b < payload_cr_plus4; ++b) cw = (cw << 1) | deint_pay[i + b];
        auto dec = lora::utils::hamming_decode4(cw, payload_cr_plus4, hdr_opt->cr, T);
        if (!dec) { fec_failed = true; nibbles[nib_idx++] = 0u; }
        else { nibbles[nib_idx++] = dec->first & 0x0F; }
        if (nib_idx >= pay_crc_bytes * 2) break;
    }
    
    // Debug payload nibbles
    printf("DEBUG: Payload nibbles: ");
    for (size_t i = 0; i < nib_idx; ++i) {
        printf("0x%x ", nibbles[i]);
    }
    printf("\n");
    std::vector<uint8_t> pay(pay_crc_bytes);
    for (size_t i = 0; i < pay_crc_bytes; ++i) {
        uint8_t low  = (i*2   < nib_idx) ? nibbles[i*2]     : 0u;
        uint8_t high = (i*2+1 < nib_idx) ? nibbles[i*2 + 1] : 0u;
        pay[i] = (uint8_t)((high << 4) | low);
    }
    // Save pre-dewhitening bytes (A3)
    ws.dbg_predew = pay;
    
    // Debug payload bytes before dewhitening
    printf("DEBUG: Payload bytes before dewhitening: ");
    for (size_t i = 0; i < pay.size(); ++i) printf("0x%02x ", pay[i]);
    printf("\n");

    // Dewhiten payload ONLY (not CRC), PN9 like GR
    auto pay_dw = pay;
    {
        auto lfsr2 = lora::utils::LfsrWhitening::pn9_default();
        if (payload_len > 0) lfsr2.apply(pay_dw.data(), payload_len);
    }

    // Show after dewhitening
    printf("DEBUG: Payload bytes after dewhitening (payload only): ");
    for (size_t i = 0; i < pay_dw.size(); ++i) printf("0x%02x ", pay_dw[i]);
    printf("\n");

    // Verify CRC both BE/LE on dewhitened payload + raw CRC trailer and record instrumentation
    lora::utils::Crc16Ccitt c;
    uint8_t crc_lo = pay[payload_len];
    uint8_t crc_hi = pay[payload_len + 1];
    ws.dbg_crc_rx_le = static_cast<uint16_t>(crc_lo) | (static_cast<uint16_t>(crc_hi) << 8);
    ws.dbg_crc_rx_be = static_cast<uint16_t>(crc_hi) << 8 | static_cast<uint16_t>(crc_lo);
    uint16_t crc_calc = c.compute(pay_dw.data(), payload_len);
    ws.dbg_crc_calc = crc_calc;
    ws.dbg_crc_ok_le = (crc_calc == ws.dbg_crc_rx_le);
    ws.dbg_crc_ok_be = (crc_calc == ws.dbg_crc_rx_be);
    // Also compute with init=0x0000
    lora::utils::Crc16Ccitt c0; c0.init = 0x0000; c0.poly = c.poly; c0.xorout = c.xorout; c0.ref_in = c.ref_in; c0.ref_out = c.ref_out;
    uint16_t crc_calc0 = c0.compute(pay_dw.data(), payload_len);
    ws.dbg_crc_calc_init0000 = crc_calc0;
    ws.dbg_crc_ok_le_init0000 = (crc_calc0 == ws.dbg_crc_rx_le);
    ws.dbg_crc_ok_be_init0000 = (crc_calc0 == ws.dbg_crc_rx_be);
    // Compute with reflection both
    lora::utils::Crc16Ccitt cref = c; cref.ref_in = true; cref.ref_out = true;
    uint16_t crc_ref = cref.compute(pay_dw.data(), payload_len);
    ws.dbg_crc_calc_refboth = crc_ref;
    ws.dbg_crc_ok_le_refboth = (crc_ref == ws.dbg_crc_rx_le);
    ws.dbg_crc_ok_be_refboth = (crc_ref == ws.dbg_crc_rx_be);
    // Compute with xorout=0xFFFF
    lora::utils::Crc16Ccitt cx = c; cx.xorout = 0xFFFF;
    uint16_t crc_x = cx.compute(pay_dw.data(), payload_len);
    ws.dbg_crc_calc_xorffff = crc_x;
    ws.dbg_crc_ok_le_xorffff = (crc_x == ws.dbg_crc_rx_le);
    ws.dbg_crc_ok_be_xorffff = (crc_x == ws.dbg_crc_rx_be);
    bool ok_be = ws.dbg_crc_ok_be;
    printf("DEBUG: CRC calc=0x%04x rx_le=0x%04x rx_be=0x%04x ok_le=%s ok_be=%s; calc0=0x%04x ok0_le=%s ok0_be=%s; cref=0x%04x okref_le=%s okref_be=%s; cxor=0x%04x okxor_le=%s okxor_be=%s\n",
           (unsigned)crc_calc, (unsigned)ws.dbg_crc_rx_le, (unsigned)ws.dbg_crc_rx_be,
           ws.dbg_crc_ok_le?"true":"false", ws.dbg_crc_ok_be?"true":"false",
           (unsigned)crc_calc0, ws.dbg_crc_ok_le_init0000?"true":"false", ws.dbg_crc_ok_be_init0000?"true":"false",
           (unsigned)crc_ref, ws.dbg_crc_ok_le_refboth?"true":"false", ws.dbg_crc_ok_be_refboth?"true":"false",
           (unsigned)crc_x, ws.dbg_crc_ok_le_xorffff?"true":"false", ws.dbg_crc_ok_be_xorffff?"true":"false");

    if (fec_failed) { lora::debug::set_fail(111); return {std::span<uint8_t>{}, false}; }
    if (!ok_be) { lora::debug::set_fail(112); return {std::span<uint8_t>{}, false}; }

    auto& out = ws.rx_data; out.resize(payload_len);
    std::copy(pay_dw.begin(), pay_dw.begin() + payload_len, out.begin());
    return { std::span<uint8_t>(out.data(), payload_len), true };
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
