#include "lora/rx/frame.hpp"
#include "lora/utils/gray.hpp"
#include "lora/utils/crc.hpp"
#include "lora/utils/whitening.hpp"
#include "lora/rx/preamble.hpp"
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
    // Check sync symbol
    size_t sync_start = *pos + min_preamble_syms * N;
    if (sync_start + N > samples.size()) return {std::span<uint8_t>{}, false};
    uint32_t sync_sym = demod_symbol(ws, &samples[sync_start]);
    if (sync_sym != expected_sync) return {std::span<uint8_t>{}, false};

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
    if (!det) return {std::span<uint8_t>{}, false};
    auto decim = decimate_os_phase(samples, det->os, det->phase);
    size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
    if (start_decim >= decim.size()) return {std::span<uint8_t>{}, false};
    auto aligned = std::span<const std::complex<float>>(decim.data() + start_decim,
                                                        decim.size() - start_decim);
    return decode_frame_with_preamble_cfo_sto(ws, aligned, sf, cr, payload_len, min_preamble_syms, expected_sync);
}

} // namespace lora::rx
