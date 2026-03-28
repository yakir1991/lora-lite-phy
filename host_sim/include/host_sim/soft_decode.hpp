#pragma once

#include <cstdint>
#include <vector>

namespace host_sim
{

// Per-symbol soft information: sf_app LLRs (one per bit, MSB first).
// Positive LLR → bit more likely 1; negative → more likely 0.
using SymbolLLR = std::vector<float>;

// Compute per-bit LLRs from FFT magnitude² array (N = 2^sf bins).
// Uses max-log approximation: LLR[i] = max(|Y[n]|² : bit i=1) - max(|Y[n]|² : bit i=0)
// where the bit mapping accounts for LoRa's (sym-1), integer CFO correction,
// optional LDRO >>2, and gray encoding.
SymbolLLR compute_symbol_llrs(const float* fft_mag_sq, int sf,
                              bool is_header_or_ldro, int cfo_int = 0);

// Soft deinterleave: takes cw_len SymbolLLRs (each with sf_app floats),
// produces sf_app soft codewords (each with cw_len floats).
std::vector<std::vector<float>> deinterleave_soft(
    const std::vector<SymbolLLR>& symbol_llrs,
    int sf, int cr, bool is_header, bool ldro,
    std::size_t& consumed);

// Soft Hamming decode: given soft codeword (cw_len LLRs), find the ML
// data nibble by scoring all 16 possible codewords.
uint8_t hamming_decode_soft(const std::vector<float>& cw_llrs, int cr_app);

// Convenience: soft-decode a block of symbols, returning nibbles.
std::vector<uint8_t> soft_decode_block(
    const std::vector<SymbolLLR>& symbol_llrs,
    int sf, int cr, bool is_header, bool ldro,
    std::size_t& consumed);

} // namespace host_sim
