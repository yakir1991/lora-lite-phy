#include "lora/utils/hamming.hpp"
#include <cassert>

namespace lora::utils {

static uint8_t parity(uint32_t v) {
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v &= 0xFu;
    static constexpr uint8_t lut[16] = {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0};
    return lut[v];
}

HammingTables make_placeholder_tables() {
    HammingTables T;
    for (int d = 0; d < 16; ++d) {
        uint8_t p  = parity(d);
        uint8_t p1 = parity(d ^ 0b1011);
        uint8_t p2 = parity(d ^ 0b0110);
        uint8_t p3 = parity(d ^ 0b1101);
        T.enc_45[d] = ((d & 0xF) | (p  << 4));
        T.enc_46[d] = ((d & 0xF) | (p  << 4) | (p1 << 5));
        T.enc_47[d] = ((d & 0xF) | (p  << 4) | (p1 << 5) | (p2 << 6));
        T.enc_48[d] = ((d & 0xF) | (p  << 4) | (p1 << 5) | (p2 << 6) | (p3 << 7));
    }
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

std::optional<std::pair<uint8_t, bool>> hamming_decode4(uint16_t codeword, uint8_t, CodeRate, const HammingTables&) {
    uint8_t nibble = codeword & 0xF;
    return std::make_optional(std::make_pair(nibble, false));
}

} // namespace lora::utils
