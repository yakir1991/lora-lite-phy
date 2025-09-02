#include "lora/rx/loopback_rx.hpp"
#include "lora/utils/whitening.hpp"
#include "lora/utils/interleaver.hpp"
#include "lora/utils/gray.hpp"
#include "lora/utils/crc.hpp"
#include <cmath>

using namespace lora::utils;

namespace lora::rx {

std::pair<std::vector<uint8_t>, bool> loopback_rx(Workspace& ws,
                                                  const std::vector<std::complex<float>>& samples,
                                                  uint32_t sf,
                                                  CodeRate cr,
                                                  size_t payload_len) {
    ws.init(sf);
    uint32_t N = ws.N;
    uint32_t cr_plus4 = static_cast<uint32_t>(cr) + 4;
    size_t nsym = samples.size() / N;

    // Demodulate symbols
    std::vector<uint32_t> symbols;
    symbols.reserve(nsym);
    for (size_t s_idx = 0; s_idx < nsym; ++s_idx) {
        const std::complex<float>* block = &samples[s_idx * N];
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
        symbols.push_back(gray_decode(max_bin));
    }

    // Symbols -> bits
    std::vector<uint8_t> bits;
    bits.reserve(symbols.size() * sf);
    for (uint32_t sym : symbols) {
        for (uint32_t b = 0; b < sf; ++b)
            bits.push_back((sym >> b) & 1);
    }

    // Deinterleave
    InterleaverMap M = make_diagonal_interleaver(sf, cr_plus4);
    std::vector<uint8_t> deint(bits.size());
    for (size_t off = 0; off < bits.size(); off += M.n_in) {
        for (uint32_t i = 0; i < M.n_out; ++i)
            deint[off + M.map[i]] = bits[off + i];
    }

    // Hamming decode
    static HammingTables T = make_hamming_tables();
    std::vector<uint8_t> nibbles;
    for (size_t i = 0; i < deint.size(); i += cr_plus4) {
        uint16_t cw = 0;
        for (uint32_t b = 0; b < cr_plus4; ++b)
            cw = (cw << 1) | deint[i + b];
        auto dec = hamming_decode4(cw, cr_plus4, cr, T);
        if (!dec)
            return {{}, false};
        nibbles.push_back(dec->first & 0x0F);
    }

    // Combine nibbles -> bytes
    std::vector<uint8_t> data((nibbles.size() + 1) / 2);
    for (size_t i = 0; i < data.size(); ++i) {
        uint8_t low  = (i * 2 < nibbles.size()) ? nibbles[i * 2] : 0;
        uint8_t high = (i * 2 + 1 < nibbles.size()) ? nibbles[i * 2 + 1] : 0;
        data[i] = (high << 4) | low;
    }
    size_t total_needed = payload_len + 2;
    if (data.size() < total_needed)
        return {{}, false};
    data.resize(total_needed);

    // Dewhiten
    auto lfsr = LfsrWhitening::pn9_default();
    lfsr.apply(data.data(), data.size());

    // CRC verify
    Crc16Ccitt crc16;
    auto ver = crc16.verify_with_trailer_be(data.data(), payload_len + 2);
    if (!ver.first)
        return {{}, false};
    data.resize(payload_len);
    return {data, true};
}

} // namespace lora::rx

