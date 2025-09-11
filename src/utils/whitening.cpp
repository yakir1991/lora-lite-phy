#include "lora/utils/whitening.hpp"

namespace lora::utils {

// PN9 LFSR: x^9 + x^5 + 1 (9-bit state). We shift right and feed back into bit 8.
// Output bit is LSB of the current state. One byte mask is produced by 8 LFSR steps, MSB-first.
static inline uint16_t lfsr9_next(uint16_t s) {
    uint16_t fb = ((s & 0x1u) ^ ((s >> 4) & 0x1u)) & 0x1u; // taps at bit 0 and bit 4
    s = static_cast<uint16_t>((s >> 1) | (fb << 8));
    return static_cast<uint16_t>(s & 0x1FFu);
}

LfsrWhitening LfsrWhitening::pn9_default() {
    LfsrWhitening w;
    w.poly = 0;          // unused
    w.state = 0x1FF;     // 9-bit non-zero seed
    w.order = 9;
    return w;
}

void LfsrWhitening::apply(uint8_t* buf, size_t len) {
    uint16_t s = static_cast<uint16_t>((state == 0) ? 0x1FFu : (state & 0x1FFu));
    for (size_t i = 0; i < len; ++i) {
        uint8_t mask = 0;
        for (int b = 0; b < 8; ++b) {
            uint8_t bit = static_cast<uint8_t>(s & 0x1u);
            mask |= static_cast<uint8_t>(bit << (7 - b)); // MSB-first
            s = lfsr9_next(s);
        }
        buf[i] ^= mask;
    }
    state = s;
}

} // namespace lora::utils
