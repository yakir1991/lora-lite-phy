#include "hamming.hpp"
#include <limits>
// LoRa-specific Hamming-like code (variable rate). Matches gr-lora-sdr logic.

namespace lora_lite {

HammingTables build_hamming_tables() { // Build LoRa codeword LUTs as in gr-lora-sdr
    HammingTables t;
    // For CR 4/5 (cr_app=1) generator (data bits d3..d0, p parity) output pattern (MSB first): d3 d2 d1 d0 p
    // Use parity p = d3^d2^d1^d0 -> codeword bits store as integer with MSB first packing
    for (int n=0;n<16;++n) {
        int d3=(n>>3)&1,d2=(n>>2)&1,d1=(n>>1)&1,d0=n&1;
        int p = d3^d2^d1^d0;
        uint8_t cw5 = (d3<<4)|(d2<<3)|(d1<<2)|(d0<<1)|p; // 5 bits in lower 5 positions
        t.enc5[n]=cw5;
        // CR>=4/6 use 4 parity bits p0..p3 per gr-lora-sdr:
        // p0 = d3^d2^d1
        // p1 = d2^d1^d0
        // p2 = d3^d2^d0
        // p3 = d3^d1^d0
        int p0 = d3^d2^d1;
        int p1 = d2^d1^d0;
        int p2 = d3^d2^d0;
        int p3 = d3^d1^d0;
        uint8_t full8 = (d3<<7)|(d2<<6)|(d1<<5)|(d0<<4)|(p0<<3)|(p1<<2)|(p2<<1)|p3; // 8-bit pattern
        // Derive cr-specific cropped versions by shifting right (4-cr_app) like encoder does ( >> (4 - cr_app) )
        // cr_app=2 (4/6): keep top 6 bits
        t.enc6[n] = full8 >> 2; // drop 2 LSB parity bits
        // cr_app=3 (4/7): keep top 7 bits
        t.enc7[n] = full8 >> 1; // drop 1 LSB parity bit
        // cr_app=4 (4/8): keep all 8 bits
        t.enc8[n] = full8;
    }
    return t;
}

static uint8_t dist(uint32_t a, uint32_t b) { return __builtin_popcount(a^b); }

uint8_t hamming_encode4(uint8_t nibble, CodeRate cr, const HammingTables& t) {
    uint8_t d = nibble & 0xF;
    switch (cr) {
        case CodeRate::CR45: return t.enc5[d];
        case CodeRate::CR46: return t.enc6[d];
        case CodeRate::CR47: return t.enc7[d];
        case CodeRate::CR48: default: return t.enc8[d];
    }
}

std::optional<uint8_t> hamming_decode4(uint8_t code, CodeRate cr, const HammingTables& t, bool allow_correct) {
    const std::array<uint8_t,16>* table=nullptr; int cw_len=0;
    switch (cr) {
        case CodeRate::CR45: table=&t.enc5; cw_len=5; break;
        case CodeRate::CR46: table=&t.enc6; cw_len=6; break;
        case CodeRate::CR47: table=&t.enc7; cw_len=7; break;
        case CodeRate::CR48: table=&t.enc8; cw_len=8; break;
    }
    uint8_t mask = static_cast<uint8_t>((1u<<cw_len)-1u);
    code &= mask;
    // Direct LUT match
    for (int n=0;n<16;++n) if ((*table)[n]==code) return static_cast<uint8_t>(n);
    if (!allow_correct) return std::nullopt;
    // Brute-force minimal Hamming distance among possible codewords
    int best=10; int best_n=-1;
    for (int n=0;n<16;++n) {
        int d = dist((*table)[n], code);
        if (d < best) { best=d; best_n=n; }
    }
    if (best<=2 && best_n>=0) return static_cast<uint8_t>(best_n); // allow up to 2-bit correction (soft guard)
    return std::nullopt;
}

} // namespace lora_lite
