#include "lora/rx/frame.hpp"
#include "lora/rx/preamble.hpp"
#include "lora/utils/gray.hpp"
#include "lora/utils/crc.hpp"
#include "lora/utils/whitening.hpp"
#include <vector>

namespace lora::rx {

static uint32_t demod_symbol_local(Workspace& ws, const std::complex<float>* block) {
    uint32_t N = ws.N;
    for (uint32_t n = 0; n < N; ++n)
        ws.rxbuf[n] = block[n] * ws.downchirp[n];
    ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
    uint32_t max_bin = 0; float max_mag = 0.f;
    for (uint32_t k = 0; k < N; ++k) {
        float mag = std::norm(ws.fftbuf[k]);
        if (mag > max_mag) { max_mag = mag; max_bin = k; }
    }
    return max_bin;
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
    uint32_t net1 = ((expected_sync & 0xF0u) >> 4) << 3;
    uint32_t net2 = (expected_sync & 0x0Fu) << 3;
    size_t sync_start = 0;
    bool found_sync = false;
    int sym_shifts_os1[5] = {0, -1, 1, -2, 2};
    int samp_shifts_os1[5] = {0, -(int)N/32, (int)N/32, -(int)N/16, (int)N/16};
    for (int s : sym_shifts_os1) {
        size_t base = (s >= 0) ? (*pos + (min_preamble_syms + (size_t)s) * N)
                               : (*pos + (min_preamble_syms - (size_t)(-s)) * N);
        for (int so : samp_shifts_os1) {
            if (so >= 0) {
                if (base + (size_t)so + N > samples.size()) continue;
                size_t idx = base + (size_t)so;
                uint32_t ss = demod_symbol_local(ws, &samples[idx]);
                if (std::abs(int(ss) - int(net1)) <= 2 || std::abs(int(ss) - int(net2)) <= 2) { found_sync = true; sync_start = idx; break; }
            } else {
                size_t offs = (size_t)(-so);
                if (base < offs) continue;
                size_t idx = base - offs;
                if (idx + N > samples.size()) continue;
                uint32_t ss = demod_symbol_local(ws, &samples[idx]);
                if (std::abs(int(ss) - int(net1)) <= 2 || std::abs(int(ss) - int(net2)) <= 2) { found_sync = true; sync_start = idx; break; }
            }
        }
        if (found_sync) break;
    }
    if (!found_sync) {
        std::vector<std::complex<float>> ref(N);
        for (uint32_t n = 0; n < N; ++n)
            ref[n] = std::conj(ws.upchirp[(n + net1) % N]);
        long best_off = 0; float best_mag = -1.f;
        int range = (int)N/8; int step = std::max<int>(1, (int)N/64);
        size_t base = *pos + min_preamble_syms * N;
        for (int off = -range; off <= range; off += step) {
            if (off >= 0) {
                if (base + (size_t)off + N > samples.size()) continue;
                size_t idx = base + (size_t)off;
                std::complex<float> acc(0.f,0.f);
                for (uint32_t n = 0; n < N; ++n) acc += samples[idx + n] * ref[n];
                float mag = std::abs(acc);
                if (mag > best_mag) { best_mag = mag; best_off = off; }
            } else {
                size_t offs = (size_t)(-off);
                if (base < offs) continue;
                size_t idx = base - offs;
                if (idx + N > samples.size()) continue;
                std::complex<float> acc(0.f,0.f);
                for (uint32_t n = 0; n < N; ++n) acc += samples[idx + n] * ref[n];
                float mag = std::abs(acc);
                if (mag > best_mag) { best_mag = mag; best_off = off; }
            }
        }
        if (best_mag > 0.f) {
            sync_start = (best_off >= 0) ? (base + (size_t)best_off)
                                        : (base - (size_t)(-best_off));
            found_sync = true;
        }
    }
    if (!found_sync) return {std::span<uint8_t>{}, false};

    auto data = std::span<const std::complex<float>>(samples.data() + sync_start + N,
                                                     samples.size() - (sync_start + N));

    uint32_t cr_plus4 = static_cast<uint32_t>(cr) + 4;
    size_t frame_bytes = 4 + payload_len + 2;
    size_t needed_bits = frame_bytes * 2 * cr_plus4;
    uint32_t block_bits = sf * cr_plus4;
    if (needed_bits % block_bits) needed_bits = ((needed_bits / block_bits) + 1) * block_bits;
    size_t nsym = needed_bits / sf;
    if (data.size() < nsym * N) return {std::span<uint8_t>{}, false};

    ws.ensure_rx_buffers(nsym, sf, cr_plus4);
    auto& symbols = ws.rx_symbols;
    for (size_t s = 0; s < nsym; ++s) {
        uint32_t raw_symbol = demod_symbol_local(ws, &data[s * N]);
        symbols[s] = lora::utils::gray_encode(raw_symbol);
    }
    auto& bits = ws.rx_bits;
    size_t bit_idx = 0;
    for (size_t s = 0; s < nsym; ++s) {
        uint32_t sym = symbols[s];
        for (uint32_t b = 0; b < sf; ++b) bits[bit_idx++] = (sym >> b) & 1;
    }
    const auto& M = ws.get_interleaver(sf, cr_plus4);
    auto& deint = ws.rx_deint;
    for (size_t off = 0; off < bit_idx; off += M.n_in)
        for (uint32_t i = 0; i < M.n_out; ++i) deint[off + M.map[i]] = bits[off + i];

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
    auto& frame = ws.rx_data;
    size_t frame_len = (nib_idx + 1) / 2;
    frame.resize(frame_len);
    for (size_t i = 0; i < frame_len; ++i) {
        uint8_t low  = (i * 2 < nib_idx) ? nibbles[i * 2] : 0;
        uint8_t high = (i * 2 + 1 < nib_idx) ? nibbles[i * 2 + 1] : 0;
        frame[i] = (high << 4) | low;
    }
    auto lfsr = lora::utils::LfsrWhitening::pn9_default();
    lfsr.apply(frame.data(), frame_len);
    if (frame_len < 4) return {std::span<uint8_t>{}, false};
    auto hdr_opt = parse_local_header_with_crc(frame.data(), 4);
    if (!hdr_opt) return {std::span<uint8_t>{}, false};
    if (hdr_opt->payload_len != payload_len) return {std::span<uint8_t>{}, false};
    if (frame_len < 4 + payload_len + 2) return {std::span<uint8_t>{}, false};
    lora::utils::Crc16Ccitt c;
    auto ok = c.verify_with_trailer_be(frame.data() + 4, payload_len + 2);
    if (!ok.first) return {std::span<uint8_t>{}, false};
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
    auto det = detect_preamble_os(ws, samples, sf, min_preamble_syms, {4,2,1,8});
    if (!det) { lora::debug::set_fail(100); return {std::span<uint8_t>{}, false}; }
    auto decim = decimate_os_phase(samples, det->os, det->phase);
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

} // namespace lora::rx



