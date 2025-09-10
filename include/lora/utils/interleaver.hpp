#pragma once
#include <cstdint>
#include <vector>

namespace lora::utils {

struct InterleaverMap {
    std::vector<uint32_t> map; ///< map[k] gives the source bit index
    uint32_t n_in{};           ///< number of bits before interleaving
    uint32_t n_out{};          ///< number of bits after interleaving (same as n_in)
};

/**
 * Build the LoRa diagonal interleaver map.
 *
 * The interleaver operates on an \f$sf \times cr\f$ table (rows by columns).
 * Columns are cyclically shifted by their index; when `sf > 6` an additional
 * shift of one column (Low Data Rate Optimization) is applied before the table
 * is flattened row-wise.  Both input and output sequences therefore contain
 * `sf * cr_plus4` bits.
 *
 * @param sf         Spreading factor (number of rows).
 * @param cr_plus4   Coding rate parameter (4+CR) giving number of columns.
 */
InterleaverMap make_diagonal_interleaver(uint32_t sf, uint32_t cr_plus4);

} // namespace lora::utils
