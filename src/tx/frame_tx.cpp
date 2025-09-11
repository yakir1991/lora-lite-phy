#include "lora/tx/frame_tx.hpp"
#include "lora/utils/whitening.hpp"
#include "lora/utils/gray.hpp"
#include "lora/utils/crc.hpp"

namespace lora::tx {

std::span<const std::complex<float>> frame_tx(Workspace& ws,
                                              const std::vector<uint8_t>& payload,
                                              uint32_t sf,
                                              lora::utils::CodeRate cr,
                                              const lora::rx::LocalHeader& hdr) {
    ws.init(sf);
    uint32_t N = ws.N;
    uint32_t cr_plus4 = static_cast<uint32_t>(cr) + 4;

    // Build STANDARD LoRa header (5 bytes, nibble-coded bytes) + payload + payload CRC
    // Nibbles: [len_hi, len_lo, flags(4b: has_crc | (cr_idx<<1)), chk_msb1bit, chk_low4bits]
    auto cr_idx = [&](){
        switch (cr) {
            case lora::utils::CodeRate::CR45: return (uint8_t)1;
            case lora::utils::CodeRate::CR46: return (uint8_t)2;
            case lora::utils::CodeRate::CR47: return (uint8_t)3;
            case lora::utils::CodeRate::CR48: return (uint8_t)4;
        }
        return (uint8_t)1;
    }();
    const uint8_t len = static_cast<uint8_t>(payload.size() & 0xFF);
    uint8_t n0 = (len >> 4) & 0x0F;     // high nibble
    uint8_t n1 = (len & 0x0F);          // low nibble
    uint8_t flags = ((cr_idx & 0x7u) << 1) | (hdr.has_crc ? 1u : 0u); // 4-bit value
    uint8_t n2 = flags & 0x0F;
    // Compute header checksum bits c4..c0 exactly like RX parse_standard_lora_header
    bool c4 = ((n0 & 0b1000) >> 3) ^ ((n0 & 0b0100) >> 2) ^ ((n0 & 0b0010) >> 1) ^ (n0 & 0b0001);
    bool c3 = ((n0 & 0b1000) >> 3) ^ ((n1 & 0b1000) >> 3) ^ ((n1 & 0b0100) >> 2) ^ ((n1 & 0b0010) >> 1) ^ (n2 & 0b0001);
    bool c2 = ((n0 & 0b0100) >> 2) ^ ((n1 & 0b1000) >> 3) ^ (n1 & 0b0001) ^ ((n2 & 0b1000) >> 3) ^ ((n2 & 0b0010) >> 1);
    bool c1 = ((n0 & 0b0010) >> 1) ^ ((n1 & 0b0100) >> 2) ^ (n1 & 0b0001) ^ ((n2 & 0b0100) >> 2) ^ ((n2 & 0b0010) >> 1) ^ (n2 & 0b0001);
    bool c0 = (n0 & 0b0001) ^ ((n1 & 0b0010) >> 1) ^ ((n2 & 0b1000) >> 3) ^ ((n2 & 0b0100) >> 2) ^ ((n2 & 0b0010) >> 1) ^ (n2 & 0b0001);
    uint8_t n3 = c4 ? 1u : 0u;           // only bit0 used
    uint8_t n4 = (uint8_t)((c3 << 3) | (c2 << 2) | (c1 << 1) | c0) & 0x0F;
    // Header bytes are nibble-coded as single-nibble bytes
    std::vector<uint8_t> hdr_bytes = { n0, n1, n2, n3, n4 };

    // Payload CRC trailer (big endian)
    lora::utils::Crc16Ccitt crc16;
    auto trailer = crc16.make_trailer_be(payload.data(), payload.size());

    std::vector<uint8_t> frame;
    frame.reserve(hdr_bytes.size() + payload.size() + 2);
    frame.insert(frame.end(), hdr_bytes.begin(), hdr_bytes.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(trailer.first);
    frame.push_back(trailer.second);

    // Whitening: apply PN9 to payload bytes ONLY (do not whiten header or CRC trailer)
    auto lfsr = lora::utils::LfsrWhitening::pn9_default();
    if (!payload.empty())
        lfsr.apply(frame.data() + hdr_bytes.size(), payload.size());

    // Prepare header and payload segments separately (header uses CR=4/8 per LoRa spec)
    static lora::utils::HammingTables T = lora::utils::make_hamming_tables();
    auto emit_bits_for_bytes = [&](const std::vector<uint8_t>& bytes,
                                   lora::utils::CodeRate cr_use,
                                   std::vector<uint8_t>& out_bits) {
        uint32_t crp4 = static_cast<uint32_t>(cr_use) + 4;
        out_bits.clear(); out_bits.reserve(bytes.size() * 2 * crp4);
        for (uint8_t b : bytes) {
            uint8_t n1 = b & 0x0F;
            uint8_t n2 = b >> 4;
            auto enc1 = lora::utils::hamming_encode4(n1, cr_use, T);
            auto enc2 = lora::utils::hamming_encode4(n2, cr_use, T);
            for (int i = enc1.second - 1; i >= 0; --i) out_bits.push_back((enc1.first >> i) & 1);
            for (int i = enc2.second - 1; i >= 0; --i) out_bits.push_back((enc2.first >> i) & 1);
        }
    };

    // Split frame into header / payload+crc
    std::vector<uint8_t> hdr_vec(frame.begin(), frame.begin() + (ptrdiff_t)hdr_bytes.size());
    std::vector<uint8_t> pay_vec(frame.begin() + (ptrdiff_t)hdr_bytes.size(), frame.end());

    // Header symbols (GR-style mapping) using sf_app rows and two blocks to cover 10 codewords
    const uint32_t header_cr_plus4 = 8u;
    const uint32_t sf_app = (sf >= 2) ? (sf - 2) : sf;
    const uint32_t cw_len_hdr = header_cr_plus4; // 8

    // Prepare 10 header nibbles across rows (two blocks of sf_app rows)
    // Simple row-major order expected by RX flattening: [len_hi, len_lo, flags, c4, c3..c0, 0, 0, 0, 0, 0]
    std::array<uint8_t,10> hdr_nibbles10{
        static_cast<uint8_t>(n0 & 0x0F),
        static_cast<uint8_t>(n1 & 0x0F),
        static_cast<uint8_t>(n2 & 0x0F),
        static_cast<uint8_t>(n3 & 0x01),
        static_cast<uint8_t>(n4 & 0x0F),
        static_cast<uint8_t>(0u),
        static_cast<uint8_t>(0u),
        static_cast<uint8_t>(0u),
        static_cast<uint8_t>(0u),
        static_cast<uint8_t>(0u)
    };

    auto encode_nibble_cr48_msb = [&](uint8_t nibble) -> std::array<uint8_t,8> {
        std::array<uint8_t,8> out{};
        auto enc = lora::utils::hamming_encode4(static_cast<uint8_t>(nibble & 0x0F), lora::utils::CodeRate::CR48, T);
        for (int b = 7; b >= 0; --b) {
            size_t col = static_cast<size_t>(7 - b);
            out[col] = static_cast<uint8_t>((enc.first >> b) & 1u);
        }
        return out;
    };

    // Pre-encode the 10 codewords
    std::array<std::array<uint8_t,8>,10> cw_list{};
    for (size_t i = 0; i < cw_list.size(); ++i) cw_list[i] = encode_nibble_cr48_msb(hdr_nibbles10[i]);

    // Number of header blocks needed to cover 10 codewords with sf_app rows per block
    const uint32_t num_blocks = static_cast<uint32_t>((10u + sf_app - 1u) / sf_app);
    std::vector<uint32_t> hdr_syms; hdr_syms.reserve(num_blocks * cw_len_hdr);
    for (uint32_t blk = 0; blk < num_blocks; ++blk) {
        // Build deinter_bin (sf_app rows x 8 cols) from cw_list slice
        std::vector<std::vector<uint8_t>> deinter_bin(sf_app, std::vector<uint8_t>(cw_len_hdr, 0));
        for (uint32_t r = 0; r < sf_app; ++r) {
            size_t idx = static_cast<size_t>(blk) * static_cast<size_t>(sf_app) + static_cast<size_t>(r);
            if (idx < cw_list.size()) {
                for (uint32_t c = 0; c < cw_len_hdr; ++c)
                    deinter_bin[r][c] = cw_list[idx][c];
            } else {
                for (uint32_t c = 0; c < cw_len_hdr; ++c) deinter_bin[r][c] = 0u;
            }
        }
        // Apply GR diagonal mapping inverse: deinter_bin[mod(i - j - 1, sf_app)][i] = inter_bin[i][j]
        std::vector<std::vector<uint8_t>> inter_bin(cw_len_hdr, std::vector<uint8_t>(sf_app, 0));
        for (uint32_t i = 0; i < cw_len_hdr; ++i) {
            for (uint32_t j = 0; j < sf_app; ++j) {
                int r = static_cast<int>(i) - static_cast<int>(j) - 1;
                r %= static_cast<int>(sf_app);
                if (r < 0) r += static_cast<int>(sf_app);
                inter_bin[i][j] = deinter_bin[static_cast<size_t>(r)][i];
            }
        }
        // Produce 8 symbols for this block
        for (uint32_t i = 0; i < cw_len_hdr; ++i) {
            uint32_t gnu = 0;
            for (uint32_t j = 0; j < sf_app; ++j) gnu = (gnu << 1) | (inter_bin[i][j] & 1u);
            uint32_t g = ((gnu << 2) + 1u) & ((1u << sf) - 1u);
            uint32_t corr = lora::utils::gray_decode(g);
            uint32_t sym  = (corr + 44u) % N;
            hdr_syms.push_back(sym & (N - 1));
        }
    }
    size_t nsym_hdr = hdr_syms.size();

    // Payload bits and interleaving
    std::vector<uint8_t> bits_pay; emit_bits_for_bytes(pay_vec, cr, bits_pay);
    uint32_t block_bits_pay = sf * cr_plus4;
    size_t bit_count_pay = bits_pay.size();
    if (bit_count_pay % block_bits_pay) bit_count_pay = ((bit_count_pay / block_bits_pay) + 1) * block_bits_pay;
    bits_pay.resize(bit_count_pay, 0);
    const auto& Mp = ws.get_interleaver(sf, cr_plus4);
    std::vector<uint8_t> inter_pay(bit_count_pay);
    for (size_t off = 0; off < bit_count_pay; off += Mp.n_in)
        for (uint32_t i = 0; i < Mp.n_out; ++i)
            inter_pay[off + i] = bits_pay[off + Mp.map[i]];
    size_t nsym_pay = bit_count_pay / sf;

    // Concatenate interleaved streams and convert to symbols
    // Header: bits MSB-first → S_hdr; corr = gray_decode(S_hdr); sym = (corr + 44) % N
    // Payload: bits LSB-first directly as raw symbol (no Gray)
    size_t nsym = nsym_hdr + nsym_pay;
    ws.tx_symbols.resize(nsym);
    for (size_t i = 0; i < nsym_hdr; ++i) ws.tx_symbols[i] = hdr_syms[i];
    for (size_t i = 0; i < nsym_pay; ++i) {
        // Build payload symbol bits MSB-first, then Gray-decode to corr; payload has no +44 offset
        uint32_t S = 0;
        for (uint32_t b = 0; b < sf; ++b) S = (S << 1) | inter_pay[i * sf + b];
        uint32_t corr = lora::utils::gray_decode(S);
        ws.tx_symbols[nsym_hdr + i] = corr & (N - 1);
    }

    // Modulate symbols into chirps
    auto& out = ws.tx_iq;
    out.resize(nsym * N);
    for (size_t s_idx = 0; s_idx < nsym; ++s_idx) {
        uint32_t sym = ws.tx_symbols[s_idx] & (N - 1);
        for (uint32_t n = 0; n < N; ++n)
            out[s_idx * N + n] = ws.upchirp[(n + sym) % N];
    }

    return {out.data(), nsym * N};
}

} // namespace lora::tx

