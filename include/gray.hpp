#pragma once
#include <cstdint>

namespace lora_lite {
inline uint32_t gray_encode(uint32_t v) { return v ^ (v >> 1); }
inline uint32_t gray_decode(uint32_t g) {
    uint32_t n = g;
    while (g >>= 1) n ^= g;
    return n;
}
}
