#include "lora/utils/interleaver.hpp"
#include <stdexcept>

namespace lora::utils {

InterleaverMap make_diagonal_interleaver(uint32_t sf, uint32_t cr_plus4) {
    if (sf == 0 || cr_plus4 < 4)
        throw std::invalid_argument("invalid sf or cr_plus4");

    const uint32_t ncols = cr_plus4;
    const uint32_t nrows = sf;
    const bool ldro      = (sf > 6); // Low Data Rate Optimization shift

    InterleaverMap M;
    M.n_in  = nrows * ncols;
    M.n_out = M.n_in; // permutation
    M.map.resize(M.n_out);

    for (uint32_t r = 0; r < nrows; ++r) {
        for (uint32_t c = 0; c < ncols; ++c) {
            uint32_t shift   = ldro ? 1 : 0;
            uint32_t in_row  = (r + c + shift) % nrows;
            uint32_t out_idx = r * ncols + c;
            uint32_t in_idx  = in_row * ncols + c;
            M.map[out_idx]   = in_idx;
        }
    }

    return M;
}

} // namespace lora::utils
