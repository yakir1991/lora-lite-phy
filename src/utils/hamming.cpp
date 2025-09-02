#include "lora/utils/hamming.hpp"
#include <cassert>
#include <cstring>

namespace lora::utils {

static uint8_t parity(uint32_t v) {
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v &= 0xFu;
    static constexpr uint8_t lut[16] = {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0};
    return lut[v];
}

static uint8_t compute_syndrome(uint16_t cw, uint8_t nbits) {
    uint8_t d0 = (cw >> 0) & 1;
    uint8_t d1 = (cw >> 1) & 1;
    uint8_t d2 = (cw >> 2) & 1;
    uint8_t d3 = (cw >> 3) & 1;
    uint8_t p1 = (cw >> 4) & 1;
    uint8_t p2 = (cw >> 5) & 1;
    uint8_t p3 = (cw >> 6) & 1;
    uint8_t p0 = (cw >> 7) & 1;

    uint8_t s1 = (d0 ^ d1 ^ d3) ^ p1;
    uint8_t s2 = (nbits >= 6) ? ((d0 ^ d2 ^ d3) ^ p2) : 0;
    uint8_t s3 = (nbits >= 7) ? ((d1 ^ d2 ^ d3) ^ p3) : 0;
    uint8_t s0 = (nbits == 8) ? parity(cw & 0xFFu) : 0;

    return static_cast<uint8_t>((s0 << 3) | (s3 << 2) | (s2 << 1) | s1);
}

HammingTables make_hamming_tables() {
    HammingTables T;
    T.synd_45.fill(-1);
    T.synd_46.fill(-1);
    T.synd_47.fill(-1);
    T.synd_48.fill(-1);

    for (int d = 0; d < 16; ++d) {
        uint8_t d0 = (d >> 0) & 1;
        uint8_t d1 = (d >> 1) & 1;
        uint8_t d2 = (d >> 2) & 1;
        uint8_t d3 = (d >> 3) & 1;

        uint8_t p1 = d0 ^ d1 ^ d3;              // Hamming parity bits
        uint8_t p2 = d0 ^ d2 ^ d3;
        uint8_t p3 = d1 ^ d2 ^ d3;
        uint8_t p0 = d0 ^ d1 ^ d2 ^ d3 ^ p1 ^ p2 ^ p3; // overall parity

        T.enc_45[d] = static_cast<uint8_t>((d & 0xF) | (p1 << 4));
        T.enc_46[d] = static_cast<uint8_t>((d & 0xF) | (p1 << 4) | (p2 << 5));
        T.enc_47[d] = static_cast<uint8_t>((d & 0xF) | (p1 << 4) | (p2 << 5) | (p3 << 6));
        T.enc_48[d] = static_cast<uint8_t>((d & 0xF) | (p1 << 4) | (p2 << 5) | (p3 << 6) | (p0 << 7));
    }

    auto fill_dec = [](auto& arr, uint8_t nbits) {
        uint16_t base = 0; // encode(0) == 0
        for (int i = 0; i < nbits; ++i) {
            uint16_t cw = base ^ (1u << i);
            uint8_t syn = compute_syndrome(cw, nbits);
            if (syn < arr.size()) arr[syn] = static_cast<int8_t>(i);
        }
    };

    fill_dec(T.synd_45, 5);
    fill_dec(T.synd_46, 6);
    fill_dec(T.synd_47, 7);
    fill_dec(T.synd_48, 8);

    return T;
}

std::pair<uint16_t, uint8_t> hamming_encode4(uint8_t nibble, CodeRate cr, const HammingTables& T) {
    nibble &= 0xF;
    switch (cr) {
        case CodeRate::CR45: return { T.enc_45[nibble], 5 };
        case CodeRate::CR46: return { T.enc_46[nibble], 6 };
        case CodeRate::CR47: return { T.enc_47[nibble], 7 };
        case CodeRate::CR48: return { T.enc_48[nibble], 8 };
    }
    return {0,0};
}

std::optional<std::pair<uint8_t, bool>> hamming_decode4(uint16_t codeword, uint8_t nbits, CodeRate cr, const HammingTables& T) {
    codeword &= (1u << nbits) - 1u;
    uint8_t syn = compute_syndrome(codeword, nbits);
    uint8_t nibble = static_cast<uint8_t>(codeword & 0xF);

    switch (cr) {
        case CodeRate::CR45:
        case CodeRate::CR46:
            if (syn) return std::nullopt;
            return std::make_optional(std::make_pair(nibble, false));

        case CodeRate::CR47: {
            if (syn == 0) return std::make_optional(std::make_pair(nibble, false));
            int8_t idx = T.synd_47[syn];
            if (idx < 0) return std::nullopt;
            codeword ^= (1u << idx);
            nibble = static_cast<uint8_t>(codeword & 0xF);
            return std::make_optional(std::make_pair(nibble, true));
        }
        case CodeRate::CR48: {
            if (syn == 0) return std::make_optional(std::make_pair(nibble, false));
            int8_t idx = T.synd_48[syn];
            if (idx < 0) return std::nullopt;
            codeword ^= (1u << idx);
            nibble = static_cast<uint8_t>(codeword & 0xF);
            return std::make_optional(std::make_pair(nibble, true));
        }
    }
    return std::nullopt;
}

} // namespace lora::utils
