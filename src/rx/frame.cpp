#include "lora/rx/frame.hpp"
#include "lora/utils/gray.hpp"
#include "lora/utils/crc.hpp"
#include "lora/utils/whitening.hpp"
#include "lora/rx/preamble.hpp"
#include "lora/debug.hpp"
#include "lora/rx/decimate.hpp"

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
        if (mag > max_mag) { max_mag = mag; max_bin = k; }
    }
    return lora::utils::gray_decode(max_bin);
}

std::pair<std::span<uint8_t>, bool> decode_frame_with_preamble(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    auto pos = detect_preamble(ws, samples, sf, min_preamble_syms);
    if (!pos) return {std::span<uint8_t>{}, false};
    ws.init(sf);
    uint32_t N = ws.N;
    // Check sync symbol with small elastic search (±2 symbols)
    size_t sync_start = 0;
    bool found_sync = false;
    int shifts[5] = {0, -1, 1, -2, 2};
    for (int s : shifts) {
        if ((int)min_preamble_syms + s < 1) continue;
        size_t idx = (s >= 0) ? (*pos + (min_preamble_syms + (size_t)s) * N)
                               : (*pos + (min_preamble_syms - (size_t)(-s)) * N);
        if (idx + N > samples.size()) continue;
        uint32_t ss = demod_symbol(ws, &samples[idx]);
        if (ss == expected_sync) { found_sync = true; sync_start = idx; break; }
    }
    if (!found_sync) return {std::span<uint8_t>{}, false};

    // Data starts after sync
    auto data = std::span<const std::complex<float>>(samples.data() + sync_start + N,
                                                     samples.size() - (sync_start + N));

    // Demodulate all symbols needed to cover header(4)+payload+CRC(2)
    uint32_t cr_plus4 = static_cast<uint32_t>(cr) + 4;
    size_t frame_bytes = 4 + payload_len + 2;
    size_t needed_bits = frame_bytes * 2 * cr_plus4;
    uint32_t block_bits = sf * cr_plus4;
    if (needed_bits % block_bits) needed_bits = ((needed_bits / block_bits) + 1) * block_bits;
    size_t nsym = needed_bits / sf;
    if (data.size() < nsym * N) return {std::span<uint8_t>{}, false};

    ws.ensure_rx_buffers(nsym, sf, cr_plus4);
    auto& symbols = ws.rx_symbols;
    for (size_t s = 0; s < nsym; ++s) symbols[s] = demod_symbol(ws, &data[s * N]);
    auto& bits = ws.rx_bits;
    size_t bit_idx = 0;
    for (size_t s = 0; s < nsym; ++s) {
        uint32_t sym = symbols[s];
        for (uint32_t b = 0; b < sf; ++b) bits[bit_idx++] = (sym >> b) & 1;
    }
    // Deinterleave
    const auto& M = ws.get_interleaver(sf, cr_plus4);
    auto& deint = ws.rx_deint;
    for (size_t off = 0; off < bit_idx; off += M.n_in)
        for (uint32_t i = 0; i < M.n_out; ++i) deint[off + M.map[i]] = bits[off + i];

    // Hamming decode
    static lora::utils::HammingTables T = lora::utils::make_hamming_tables();
    auto& nibbles = ws.rx_nibbles;
    size_t nib_idx = 0;
    for (size_t i = 0; i < needed_bits; i += cr_plus4) {
        uint16_t cw = 0;
        for (uint32_t b = 0; b < cr_plus4; ++b) cw = (cw << 1) | deint[i + b];
        auto dec = lora::utils::hamming_decode4(cw, cr_plus4, cr, T);
        if (!dec) return {std::span<uint8_t>{}, false};
        nibbles[nib_idx++] = dec->first & 0x0F;
    }
    // Nibbles -> bytes
    auto& frame = ws.rx_data;
    size_t frame_len = (nib_idx + 1) / 2;
    frame.resize(frame_len);
    for (size_t i = 0; i < frame_len; ++i) {
        uint8_t low  = (i * 2 < nib_idx) ? nibbles[i * 2] : 0;
        uint8_t high = (i * 2 + 1 < nib_idx) ? nibbles[i * 2 + 1] : 0;
        frame[i] = (high << 4) | low;
    }

    // Dewhiten entire frame
    auto lfsr = lora::utils::LfsrWhitening::pn9_default();
    lfsr.apply(frame.data(), frame_len);

    // Parse header
    if (frame_len < 4) return {std::span<uint8_t>{}, false};
    auto hdr_opt = parse_local_header_with_crc(frame.data(), 4);
    if (!hdr_opt) return {std::span<uint8_t>{}, false};
    if (hdr_opt->payload_len != payload_len) return {std::span<uint8_t>{}, false};
    // Verify payload CRC
    if (frame_len < 4 + payload_len + 2) return {std::span<uint8_t>{}, false};
    lora::utils::Crc16Ccitt c;
    auto ok = c.verify_with_trailer_be(frame.data() + 4, payload_len + 2);
    if (!ok.first) return {std::span<uint8_t>{}, false};
    // Return payload span
    return { std::span<uint8_t>(frame.data() + 4, payload_len), true };
}

std::pair<std::span<uint8_t>, bool> decode_frame_with_preamble_cfo_sto(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    auto pos = detect_preamble(ws, samples, sf, min_preamble_syms);
    if (!pos) return {std::span<uint8_t>{}, false};
    auto cfo = estimate_cfo_from_preamble(ws, samples, sf, *pos, min_preamble_syms);
    if (!cfo) return {std::span<uint8_t>{}, false};
    std::vector<std::complex<float>> comp(samples.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
    std::complex<float> j(0.f, 1.f);
    for (size_t n = 0; n < samples.size(); ++n)
        comp[n] = samples[n] * std::exp(j * (two_pi_eps * static_cast<float>(n)));
    auto sto = estimate_sto_from_preamble(ws, comp, sf, *pos, min_preamble_syms, static_cast<int>(ws.N/8));
    if (!sto) return {std::span<uint8_t>{}, false};
    int shift = *sto;
    size_t aligned_start = (shift >= 0) ? (*pos + static_cast<size_t>(shift))
                                        : (*pos - static_cast<size_t>(-shift));
    if (aligned_start >= comp.size()) return {std::span<uint8_t>{}, false};
    auto aligned = std::span<const std::complex<float>>(comp.data() + aligned_start,
                                                        comp.size() - aligned_start);
    return decode_frame_with_preamble(ws, aligned, sf, cr, payload_len, min_preamble_syms, expected_sync);
}

std::pair<std::span<uint8_t>, bool> decode_frame_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    auto det = detect_preamble_os(ws, samples, sf, min_preamble_syms, {1,2,4,8});
    if (!det) { lora::debug::set_fail(100); return {std::span<uint8_t>{}, false}; }
    auto decim = decimate_os_phase(samples, det->os, det->phase);
    // Compensate decimator group delay by padding zeros at the tail so that
    // we don't lose frame symbols due to front trimming.
    {
        unsigned int L = static_cast<unsigned int>(std::max(32*det->os, 8*det->os));
        size_t pad_os1 = static_cast<size_t>((L/2) / std::max(det->os,1));
        decim.insert(decim.end(), pad_os1, std::complex<float>(0.f, 0.f));
    }
    size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
    if (start_decim >= decim.size()) { lora::debug::set_fail(101); return {std::span<uint8_t>{}, false}; }
    auto aligned = std::span<const std::complex<float>>(decim.data() + start_decim,
                                                        decim.size() - start_decim);
    return decode_frame_with_preamble_cfo_sto(ws, aligned, sf, cr, payload_len, min_preamble_syms, expected_sync);
}

// Auto-length from header: OS-aware detect/align, then decode header to get payload length,
// then decode payload+CRC and verify.
std::pair<std::span<uint8_t>, bool> decode_frame_with_preamble_cfo_sto_os_auto(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    // Detect OS and phase
    auto det = detect_preamble_os(ws, samples, sf, min_preamble_syms, {1,2,4,8});
    if (!det) return {std::span<uint8_t>{}, false};
    // Decimate to OS=1
    auto decim = decimate_os_phase(samples, det->os, det->phase);
    size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
    if (start_decim >= decim.size()) return {std::span<uint8_t>{}, false};
    auto aligned0 = std::span<const std::complex<float>>(decim.data() + start_decim,
                                                         decim.size() - start_decim);
    // Estimate CFO over preamble (OS=1 now)
    auto pos0 = detect_preamble(ws, aligned0, sf, min_preamble_syms);
    if (!pos0) { lora::debug::set_fail(102); return {std::span<uint8_t>{}, false}; }
    auto cfo = estimate_cfo_from_preamble(ws, aligned0, sf, *pos0, min_preamble_syms);
    if (!cfo) { lora::debug::set_fail(103); return {std::span<uint8_t>{}, false}; }
    // Compensate CFO
    std::vector<std::complex<float>> comp(aligned0.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
    std::complex<float> j(0.f, 1.f);
    for (size_t n = 0; n < aligned0.size(); ++n)
        comp[n] = aligned0[n] * std::exp(j * (two_pi_eps * static_cast<float>(n)));
    // Estimate integer STO
    auto sto = estimate_sto_from_preamble(ws, comp, sf, *pos0, min_preamble_syms, static_cast<int>(ws.N/8));
    if (!sto) { lora::debug::set_fail(104); return {std::span<uint8_t>{}, false}; }
    int shift = *sto;
    size_t aligned_start = (shift >= 0) ? (*pos0 + static_cast<size_t>(shift))
                                        : (*pos0 - static_cast<size_t>(-shift));
    if (aligned_start >= comp.size()) { lora::debug::set_fail(105); return {std::span<uint8_t>{}, false}; }
    auto aligned = std::span<const std::complex<float>>(comp.data() + aligned_start,
                                                        comp.size() - aligned_start);
    // Check sync word with small elastic search (±2 symbols)
    ws.init(sf);
    uint32_t N = ws.N;
    size_t sync_start = 0;
    bool found_sync2 = false;
    int shifts2[5] = {0, -1, 1, -2, 2};
    for (int s : shifts2) {
        if ((int)min_preamble_syms + s < 1) continue;
        size_t idx = (s >= 0) ? ((min_preamble_syms + (size_t)s) * N)
                               : ((min_preamble_syms - (size_t)(-s)) * N);
        if (idx + N > aligned.size()) continue;
        uint32_t ss = demod_symbol(ws, &aligned[idx]);
        if (ss == expected_sync) { found_sync2 = true; sync_start = idx; break; }
    }
    if (!found_sync2) { lora::debug::set_fail(107); return {std::span<uint8_t>{}, false}; }
    // Data starts after sync
    auto data = std::span<const std::complex<float>>(aligned.data() + sync_start + N,
                                                     aligned.size() - (sync_start + N));
    // Streamed decode across interleaver blocks to avoid per-part padding.
    const uint32_t cr_plus4 = static_cast<uint32_t>(cr) + 4;
    const uint32_t block_bits = sf * cr_plus4;     // bits per interleaver block
    const uint32_t block_syms = cr_plus4;          // symbols per interleaver block
    const size_t   total_syms = data.size() / ws.N;
    const auto& M = ws.get_interleaver(sf, cr_plus4);
    static lora::utils::HammingTables T = lora::utils::make_hamming_tables();

    std::vector<uint8_t> inter_bits(block_bits);
    std::vector<uint8_t> deint_bits(block_bits);
    std::vector<uint8_t> stream_bits; stream_bits.reserve(block_bits * 8);
    size_t sym_consumed = 0;
    auto demod_block_append = [&](void) -> bool {
        if (sym_consumed + block_syms > total_syms) return false;
        for (uint32_t s = 0; s < block_syms; ++s) {
            uint32_t sym = demod_symbol(ws, &data[(sym_consumed + s) * ws.N]);
            for (uint32_t b = 0; b < sf; ++b)
                inter_bits[s * sf + b] = (sym >> b) & 1u;
        }
        for (uint32_t i = 0; i < M.n_out; ++i)
            deint_bits[M.map[i]] = inter_bits[i];
        stream_bits.insert(stream_bits.end(), deint_bits.begin(), deint_bits.end());
        sym_consumed += block_syms;
        return true;
    };

    // Header (4 bytes)
    const size_t hdr_bytes = 4;
    const size_t hdr_bits_exact = hdr_bytes * 2 * cr_plus4;
    while (stream_bits.size() < hdr_bits_exact) {
        if (!demod_block_append()) { lora::debug::set_fail(108); return {std::span<uint8_t>{}, false}; }
    }
    std::vector<uint8_t> hdr(hdr_bytes);
    auto& nibbles = ws.rx_nibbles; nibbles.clear(); nibbles.resize(hdr_bytes * 2);
    size_t nib_idx = 0;
    for (size_t i = 0; i < hdr_bits_exact; i += cr_plus4) {
        uint16_t cw = 0; for (uint32_t b = 0; b < cr_plus4; ++b) cw = (cw << 1) | stream_bits[i + b];
        auto dec = lora::utils::hamming_decode4(cw, cr_plus4, cr, T);
        if (!dec) { lora::debug::set_fail(111); return {std::span<uint8_t>{}, false}; }
        nibbles[nib_idx++] = dec->first & 0x0F;
    }
    for (size_t i = 0; i < hdr_bytes; ++i) {
        uint8_t low  = nibbles[i*2];
        uint8_t high = nibbles[i*2+1];
        hdr[i] = (high << 4) | low;
    }
    auto lfsr = lora::utils::LfsrWhitening::pn9_default();
    lfsr.apply(hdr.data(), hdr.size());
    auto hdr_opt = parse_local_header_with_crc(hdr.data(), hdr.size());
    if (!hdr_opt) { lora::debug::set_fail(109); return {std::span<uint8_t>{}, false}; }
    const size_t payload_len = hdr_opt->payload_len;
    const size_t pay_crc_bytes = payload_len + 2;
    const size_t pay_bits_exact = pay_crc_bytes * 2 * cr_plus4;

    // Ensure enough bits for payload+CRC
    while (stream_bits.size() - hdr_bits_exact < pay_bits_exact) {
        if (!demod_block_append()) {
            // If input IQ is shorter by a small margin (e.g., decimator edge losses),
            // pad with a zero block (equivalent to encoded zero bits) to complete decode.
            // This mirrors the TX-side zero padding to the next block boundary.
            std::fill(deint_bits.begin(), deint_bits.end(), 0u);
            stream_bits.insert(stream_bits.end(), deint_bits.begin(), deint_bits.end());
            // Do not increase sym_consumed; this is virtual padding.
        }
    }
    // Decode payload
    nibbles.resize(pay_crc_bytes * 2); nib_idx = 0;
    const size_t pay_start = hdr_bits_exact;
    const size_t pay_end   = pay_start + pay_bits_exact;
    for (size_t i = pay_start; i < pay_end; i += cr_plus4) {
        uint16_t cw = 0; for (uint32_t b = 0; b < cr_plus4; ++b) cw = (cw << 1) | stream_bits[i + b];
        auto dec = lora::utils::hamming_decode4(cw, cr_plus4, cr, T);
        if (!dec) { lora::debug::set_fail(111); return {std::span<uint8_t>{}, false}; }
        nibbles[nib_idx++] = dec->first & 0x0F;
    }
    std::vector<uint8_t> pay(pay_crc_bytes);
    for (size_t i = 0; i < pay_crc_bytes; ++i) {
        uint8_t low  = nibbles[i*2];
        uint8_t high = nibbles[i*2+1];
        pay[i] = (high << 4) | low;
    }
    lfsr.apply(pay.data(), pay.size());
    lora::utils::Crc16Ccitt c;
    auto ok = c.verify_with_trailer_be(pay.data(), payload_len + 2);
    if (!ok.first) { lora::debug::set_fail(112); return {std::span<uint8_t>{}, false}; }
    auto& out = ws.rx_data; out.resize(payload_len);
    std::copy(pay.begin(), pay.begin() + payload_len, out.begin());
  return { std::span<uint8_t>(out.data(), payload_len), true };
}

} // namespace lora::rx

namespace lora::rx {

std::optional<LocalHeader> decode_header_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    // Reuse the OS-aware alignment path from decode_frame_with_preamble_cfo_sto_os_auto
    auto det = detect_preamble_os(ws, samples, sf, min_preamble_syms, {1,2,4,8});
    if (!det) return std::nullopt;
    auto decim = decimate_os_phase(samples, det->os, det->phase);
    size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
    if (start_decim >= decim.size()) return std::nullopt;
    auto aligned0 = std::span<const std::complex<float>>(decim.data() + start_decim,
                                                         decim.size() - start_decim);
    auto pos0 = detect_preamble(ws, aligned0, sf, min_preamble_syms);
    if (!pos0) return std::nullopt;
    auto cfo = estimate_cfo_from_preamble(ws, aligned0, sf, *pos0, min_preamble_syms);
    if (!cfo) return std::nullopt;
    std::vector<std::complex<float>> comp(aligned0.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
    std::complex<float> j(0.f, 1.f);
    for (size_t n = 0; n < aligned0.size(); ++n)
        comp[n] = aligned0[n] * std::exp(j * (two_pi_eps * static_cast<float>(n)));
    auto sto = estimate_sto_from_preamble(ws, comp, sf, *pos0, min_preamble_syms, static_cast<int>(ws.N/8));
    if (!sto) return std::nullopt;
    int shift = *sto;
    size_t aligned_start = (shift >= 0) ? (*pos0 + static_cast<size_t>(shift))
                                        : (*pos0 - static_cast<size_t>(-shift));
    if (aligned_start >= comp.size()) return std::nullopt;
    auto aligned = std::span<const std::complex<float>>(comp.data() + aligned_start,
                                                        comp.size() - aligned_start);
    ws.init(sf);
    uint32_t N = ws.N;
    size_t sync_start = min_preamble_syms * N;
    if (sync_start + N > aligned.size()) return std::nullopt;
    uint32_t sync_sym = demod_symbol(ws, &aligned[sync_start]);
    if (sync_sym != expected_sync) return std::nullopt;

    auto data = std::span<const std::complex<float>>(aligned.data() + sync_start + N,
                                                     aligned.size() - (sync_start + N));
    uint32_t cr_plus4 = static_cast<uint32_t>(cr) + 4;
    size_t hdr_bytes = 4;
    size_t hdr_bits = hdr_bytes * 2 * cr_plus4;
    uint32_t block_bits = sf * cr_plus4;
    if (hdr_bits % block_bits) hdr_bits = ((hdr_bits / block_bits) + 1) * block_bits;
    size_t hdr_nsym = hdr_bits / sf;
    if (data.size() < hdr_nsym * N) return std::nullopt;
    ws.ensure_rx_buffers(hdr_nsym, sf, cr_plus4);
    auto& symbols = ws.rx_symbols;
    for (size_t s = 0; s < hdr_nsym; ++s) symbols[s] = demod_symbol(ws, &data[s * N]);
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
        if (!dec) return std::nullopt;
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
    return hdr_opt;
}

std::pair<std::vector<uint8_t>, bool> decode_payload_no_crc_with_preamble_cfo_sto_os(
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
    uint32_t sync_sym = demod_symbol(ws, &aligned[sync_start]);
    if (sync_sym != expected_sync) return {{}, false};
    auto data = std::span<const std::complex<float>>(aligned.data() + sync_start + N,
                                                     aligned.size() - (sync_start + N));
    // Decode header
    uint32_t cr_plus4 = static_cast<uint32_t>(cr) + 4;
    size_t hdr_bytes = 4;
    size_t hdr_bits = hdr_bytes * 2 * cr_plus4;
    uint32_t block_bits = sf * cr_plus4;
    if (hdr_bits % block_bits) hdr_bits = ((hdr_bits / block_bits) + 1) * block_bits;
    size_t hdr_nsym = hdr_bits / sf;
    if (data.size() < hdr_nsym * N) return {{}, false};
    ws.ensure_rx_buffers(hdr_nsym, sf, cr_plus4);
    auto& symbols = ws.rx_symbols;
    for (size_t s = 0; s < hdr_nsym; ++s) symbols[s] = demod_symbol(ws, &data[s * N]);
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
    for (size_t s = 0; s < pay_nsym; ++s) symbols[s] = demod_symbol(ws, &data[(hdr_nsym + s) * N]);
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
    }
    std::vector<uint8_t> pay(pay_crc_bytes);
    for (size_t i = 0; i < pay_crc_bytes; ++i) {
        uint8_t low  = nibbles[i*2];
        uint8_t high = nibbles[i*2+1];
        pay[i] = (high << 4) | low;
    }
    // Dewhiten starting from header state
    lfsr.apply(pay.data(), pay.size());
    // Return only payload bytes (ignore CRC result)
    std::vector<uint8_t> out(payload_len);
    if (payload_len > 0 && pay.size() >= payload_len)
        std::copy(pay.begin(), pay.begin() + payload_len, out.begin());
    return { std::move(out), true };
}

} // namespace lora::rx
