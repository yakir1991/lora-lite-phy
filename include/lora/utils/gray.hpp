#pragma once
#include <cstdint>
#include <vector>

namespace lora::utils {

inline uint32_t gray_encode(uint32_t x) { return x ^ (x >> 1); }

inline uint32_t gray_decode(uint32_t g) {
    uint32_t x = g;
    for (uint32_t shift = 1; shift < 32; shift <<= 1) x ^= (x >> shift);
    return x;
}

std::vector<uint32_t> make_gray_inverse_table(uint32_t N);

} // namespace lora::utils
