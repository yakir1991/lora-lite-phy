#pragma once
#include <cstdint>
#include <vector>

namespace lora::utils {

struct InterleaverMap {
    std::vector<uint32_t> map; // map[k] = source_bit_index
    uint32_t n_in{};
    uint32_t n_out{};
};

InterleaverMap make_diagonal_interleaver(uint32_t sf, uint32_t cr_plus4);

} // namespace lora::utils
