#pragma once

#include <cstdint>

namespace host_sim
{

inline uint16_t gray_decode(uint16_t symbol)
{
    uint16_t result = symbol;
    while (symbol >>= 1u) {
        result ^= symbol;
    }
    return result;
}

} // namespace host_sim
