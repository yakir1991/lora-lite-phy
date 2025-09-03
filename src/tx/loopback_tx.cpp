#include "lora/tx/loopback_tx.hpp"
#include "lora/utils/whitening.hpp"
#include "lora/utils/gray.hpp"
#include "lora/utils/crc.hpp"
#include <cmath>

namespace lora::tx {

std::span<const std::complex<float>> loopback_tx(Workspace& ws,
                                                 const std::vector<uint8_t>& payload,
                                                 uint32_t sf,
                                                 lora::utils::CodeRate cr) {
    ws.init(sf);
    uint32_t N = ws.N;
    uint32_t cr_plus4 = static_cast<uint32_t>(cr) + 4;
    ws.ensure_tx_buffers(payload.size(), sf, cr_plus4);

    // Prepare helpers
    lora::utils::Crc16Ccitt crc16;
    auto trailer = crc16.make_trailer_be(payload.data(), payload.size());
    auto lfsr = lora::utils::LfsrWhitening::pn9_default();
    static lora::utils::HammingTables T = lora::utils::make_hamming_tables();

    // Encode payload+CRC directly into bit buffer with whitening
    auto& bits = ws.tx_bits;
    size_t bit_idx = 0;
    auto encode_byte = [&](uint8_t b) {
        lfsr.apply(&b, 1);
        uint8_t n1 = b & 0x0F;
        uint8_t n2 = b >> 4;
        auto enc1 = lora::utils::hamming_encode4(n1, cr, T);
        auto enc2 = lora::utils::hamming_encode4(n2, cr, T);
        for (int i = enc1.second - 1; i >= 0; --i)
            bits[bit_idx++] = (enc1.first >> i) & 1;
        for (int i = enc2.second - 1; i >= 0; --i)
            bits[bit_idx++] = (enc2.first >> i) & 1;
    };
    for (uint8_t b : payload)
        encode_byte(b);
    encode_byte(trailer.first);
    encode_byte(trailer.second);

    size_t nbits = bit_idx;
    uint32_t block_bits = sf * cr_plus4;
    if (nbits % block_bits) {
        size_t padded_bits = ((nbits / block_bits) + 1) * block_bits;
        for (size_t i = nbits; i < padded_bits; ++i)
            bits[i] = 0;
        nbits = padded_bits;
    }

    // Interleave
    const auto& M = ws.get_interleaver(sf, cr_plus4);
    auto& inter = ws.tx_inter;
    for (size_t off = 0; off < nbits; off += M.n_in) {
        for (uint32_t i = 0; i < M.n_out; ++i)
            inter[off + i] = bits[off + M.map[i]];
    }

    // Bits -> Gray-mapped symbols
    auto& symbols = ws.tx_symbols;
    size_t nsym = nbits / sf;
    for (size_t i = 0; i < nsym; ++i) {
        uint32_t val = 0;
        for (uint32_t b = 0; b < sf; ++b)
            val |= (uint32_t(inter[i * sf + b]) << b);
        symbols[i] = lora::utils::gray_encode(val);
    }

    // Modulate symbols into chirps
    auto& out = ws.tx_iq;
    for (size_t s_idx = 0; s_idx < nsym; ++s_idx) {
        uint32_t sym = symbols[s_idx] & (N - 1);
        for (uint32_t n = 0; n < N; ++n)
            out[s_idx * N + n] = ws.upchirp[(n + sym) % N];
    }

    return {out.data(), nsym * N};
}

} // namespace lora::tx

