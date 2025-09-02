#include "lora/tx/loopback_tx.hpp"
#include "lora/utils/whitening.hpp"
#include "lora/utils/interleaver.hpp"
#include "lora/utils/gray.hpp"
#include "lora/utils/crc.hpp"
#include <cmath>

using namespace lora::utils;

namespace lora::tx {

std::vector<std::complex<float>> loopback_tx(Workspace& ws,
                                             const std::vector<uint8_t>& payload,
                                             uint32_t sf,
                                             CodeRate cr) {
    ws.init(sf);
    uint32_t N = ws.N;
    uint32_t cr_plus4 = static_cast<uint32_t>(cr) + 4;

    // Assemble payload + CRC
    std::vector<uint8_t> data = payload;
    Crc16Ccitt crc16;
    auto trailer = crc16.make_trailer_be(data.data(), data.size());
    data.push_back(trailer.first);
    data.push_back(trailer.second);

    // Whitening
    auto lfsr = LfsrWhitening::pn9_default();
    lfsr.apply(data.data(), data.size());

    // Hamming encode -> bit vector
    static HammingTables T = make_hamming_tables();
    std::vector<uint8_t> bits;
    bits.reserve(data.size() * 2 * cr_plus4);
    for (uint8_t b : data) {
        uint8_t n1 = b & 0x0F;
        uint8_t n2 = b >> 4;
        auto enc1 = hamming_encode4(n1, cr, T);
        auto enc2 = hamming_encode4(n2, cr, T);
        for (int i = enc1.second - 1; i >= 0; --i)
            bits.push_back((enc1.first >> i) & 1);
        for (int i = enc2.second - 1; i >= 0; --i)
            bits.push_back((enc2.first >> i) & 1);
    }

    // Pad to multiple of block size
    uint32_t block_bits = sf * cr_plus4;
    if (bits.size() % block_bits)
        bits.resize((bits.size() / block_bits + 1) * block_bits, 0);

    // Interleave
    InterleaverMap M = make_diagonal_interleaver(sf, cr_plus4);
    std::vector<uint8_t> inter(bits.size());
    for (size_t off = 0; off < bits.size(); off += M.n_in) {
        for (uint32_t i = 0; i < M.n_out; ++i)
            inter[off + i] = bits[off + M.map[i]];
    }

    // Bits -> Gray-mapped symbols
    std::vector<uint32_t> symbols;
    for (size_t i = 0; i < inter.size(); i += sf) {
        uint32_t val = 0;
        for (uint32_t b = 0; b < sf; ++b)
            val |= (uint32_t(inter[i + b]) << b);
        symbols.push_back(gray_encode(val));
    }

    // Modulate symbols into chirps
    std::vector<std::complex<float>> out(symbols.size() * N);
    for (size_t s_idx = 0; s_idx < symbols.size(); ++s_idx) {
        uint32_t sym = symbols[s_idx] & (N - 1);
        for (uint32_t n = 0; n < N; ++n)
            out[s_idx * N + n] = ws.upchirp[(n + sym) % N];
    }
    return out;
}

} // namespace lora::tx

