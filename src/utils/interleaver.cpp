#include "lora/utils/interleaver.hpp"
#include <stdexcept>

namespace lora::utils {

InterleaverMap make_diagonal_interleaver(uint32_t sf, uint32_t cr_plus4) {
    InterleaverMap M;
    M.n_in  = sf;
    M.n_out = sf + (cr_plus4 - 4);
    M.map.resize(M.n_out);
    for (uint32_t i = 0; i < M.n_out; ++i) M.map[i] = (i < sf) ? i : (i % sf);
    return M;
}

} // namespace lora::utils
