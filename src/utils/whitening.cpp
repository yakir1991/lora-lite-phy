#include "lora/utils/whitening.hpp"

namespace lora::utils {

// 8-bit LFSR: x^8 + x^6 + x^5 + x^4 + 1
static inline uint8_t lfsr8_next(uint8_t s) {
    uint8_t fb = ((s >> 7) ^ (s >> 5) ^ (s >> 4) ^ (s >> 3)) & 1u;
    return (uint8_t)((s << 1) | fb);
}

LfsrWhitening LfsrWhitening::pn9_default() {
    LfsrWhitening w;
    w.poly = 0;
    w.state = 0xFF; // non-zero seed
    w.order = 8;
    return w;
}

void LfsrWhitening::apply(uint8_t* buf, size_t len) {
    uint8_t s = (state == 0) ? 0xFF : (uint8_t)state;
    for (size_t i = 0; i < len; ++i) {
        uint8_t prn = s;
        buf[i] ^= prn;
        s = lfsr8_next(s);
    }
    state = s;
}

} // namespace lora::utils
