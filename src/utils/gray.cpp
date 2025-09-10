#include "lora/utils/gray.hpp"
#include <stdexcept>

namespace lora::utils {

std::vector<uint32_t> make_gray_inverse_table(uint32_t N) {
    if ((N & (N - 1)) != 0) throw std::invalid_argument("N must be power-of-two");
    std::vector<uint32_t> inv(N);
    for (uint32_t x = 0; x < N; ++x) {
        uint32_t g = gray_encode(x);
        inv[g] = x;
    }
    return inv;
}

} // namespace lora::utils