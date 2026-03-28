#include "host_sim/soft_decode.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace host_sim
{

namespace
{

inline uint16_t gray_encode(uint16_t x)
{
    return x ^ (x >> 1);
}

// Encode a 4-bit data nibble into a (cr_app+4)-bit codeword using the same
// Hamming convention as our hard decoder (hamming.cpp).
// Returns the codeword value where bit (cw_len-1) is bits[0] (MSB).
//
// Bit layout inside the decoder:
//   bits[0..3] = data bits  (bits[0]=d0=LSB of nibble … bits[3]=d3=MSB)
//   bits[4..]  = check/parity bits
inline uint8_t hamming_encode(uint8_t nibble, int cr_app)
{
    const bool d0 = (nibble >> 0) & 1;
    const bool d1 = (nibble >> 1) & 1;
    const bool d2 = (nibble >> 2) & 1;
    const bool d3 = (nibble >> 3) & 1;

    bool bits[8] = {d0, d1, d2, d3, false, false, false, false};
    const int cw_len = cr_app + 4;

    switch (cr_app) {
    case 4:
        bits[4] = d0 ^ d1 ^ d2;
        bits[5] = d1 ^ d2 ^ d3;
        bits[6] = d0 ^ d1 ^ d3;
        bits[7] = bits[0] ^ bits[1] ^ bits[2] ^ bits[3] ^
                  bits[4] ^ bits[5] ^ bits[6];
        break;
    case 3:
        bits[4] = d0 ^ d1 ^ d2;
        bits[5] = d1 ^ d2 ^ d3;
        bits[6] = d0 ^ d1 ^ d3;
        break;
    case 2:
        bits[4] = d0 ^ d1 ^ d2;
        bits[5] = d1 ^ d2 ^ d3;
        break;
    case 1:
        bits[4] = d0 ^ d1 ^ d2 ^ d3;
        break;
    default:
        break;
    }

    // Pack: bits[0] is MSB of value (bit cw_len-1)
    uint8_t cw = 0;
    for (int k = 0; k < cw_len; ++k) {
        if (bits[k]) cw |= static_cast<uint8_t>(1u << (cw_len - 1 - k));
    }
    return cw;
}

} // anonymous namespace

// Compute per-bit LLRs for one demodulated symbol from FFT magnitude² values.
// Uses max-log approximation: LLR[i] = max(mag² where bit i = 1) - max(mag² where bit i = 0)
SymbolLLR compute_symbol_llrs(const float* fft_mag_sq, int sf,
                              bool is_header_or_ldro, int cfo_int)
{
    const int N = 1 << sf;
    const int sf_app = is_header_or_ldro ? sf - 2 : sf;
    const int divider = is_header_or_ldro ? 4 : 1;

    SymbolLLR llrs(sf_app, 0.0f);

    // For each bit position, compute max mag² where that bit is 1 vs 0.
    std::vector<float> max_one(sf_app, -std::numeric_limits<float>::infinity());
    std::vector<float> max_zero(sf_app, -std::numeric_limits<float>::infinity());

    for (int n = 0; n < N; ++n) {
        // LoRa symbol mapping: s = ((n - cfo_int - 1) mod N) / divider, then gray encode
        const uint16_t shifted = static_cast<uint16_t>(((n - cfo_int - 1 + 2 * N) % N) / divider);
        const uint16_t gray = gray_encode(shifted);
        const float mag = fft_mag_sq[n];

        for (int bit = 0; bit < sf_app; ++bit) {
            // Bit numbering: MSB first (bit 0 = MSB of sf_app-bit value)
            const int bit_val = (gray >> (sf_app - 1 - bit)) & 1;
            if (bit_val) {
                max_one[bit] = std::max(max_one[bit], mag);
            } else {
                max_zero[bit] = std::max(max_zero[bit], mag);
            }
        }
    }

    for (int bit = 0; bit < sf_app; ++bit) {
        llrs[bit] = max_one[bit] - max_zero[bit];
    }

    return llrs;
}

// Soft deinterleave: same permutation as hard but on float LLRs.
std::vector<std::vector<float>> deinterleave_soft(
    const std::vector<SymbolLLR>& symbol_llrs,
    int sf, int cr, bool is_header, bool ldro,
    std::size_t& consumed)
{
    const bool use_ldro = ldro || is_header;
    const int sf_app = use_ldro ? sf - 2 : sf;
    const int cw_len = is_header ? 8 : cr + 4;

    consumed = cw_len;

    // Build the interleave matrix (cw_len rows × sf_app cols) from symbol LLRs.
    // Each symbol contributes sf_app LLR values.
    // Same as hard: inter_matrix[i][j] = symbol_llrs[i][j]
    // Deinterleave: deinter[row][i] where row = (i - j - 1) mod sf_app
    std::vector<std::vector<float>> deinter(sf_app, std::vector<float>(cw_len, 0.0f));
    for (int i = 0; i < cw_len; ++i) {
        for (int j = 0; j < sf_app; ++j) {
            const int row = ((i - j - 1) % sf_app + sf_app) % sf_app;
            deinter[row][i] = symbol_llrs[i][j];
        }
    }

    return deinter;
}

// Soft Hamming decode: ML codeword selection.
// cw_llrs has cw_len floats (one per codeword bit, MSB first).
// Positive LLR → bit more likely 1.
uint8_t hamming_decode_soft(const std::vector<float>& cw_llrs, int cr_app)
{
    const int cw_len = cr_app + 4;

    float best_score = -std::numeric_limits<float>::infinity();
    int best_nibble = 0;

    for (int d = 0; d < 16; ++d) {
        const uint8_t cw = hamming_encode(static_cast<uint8_t>(d), cr_app);
        float score = 0.0f;
        for (int j = 0; j < cw_len; ++j) {
            // j=0 corresponds to bits[0] = MSB of codeword value
            const int cw_bit = (cw >> (cw_len - 1 - j)) & 1;
            if (cw_bit == (cw_llrs[j] > 0.0f ? 1 : 0)) {
                score += std::abs(cw_llrs[j]);
            } else {
                score -= std::abs(cw_llrs[j]);
            }
        }
        if (score > best_score) {
            best_score = score;
            best_nibble = d;
        }
    }

    return static_cast<uint8_t>(best_nibble);
}

// Convenience: soft-decode a block of symbols → nibbles.
std::vector<uint8_t> soft_decode_block(
    const std::vector<SymbolLLR>& symbol_llrs,
    int sf, int cr, bool is_header, bool ldro,
    std::size_t& consumed)
{
    auto cw_soft = deinterleave_soft(symbol_llrs, sf, cr, is_header, ldro, consumed);
    const int cr_app = is_header ? 4 : cr;
    std::vector<uint8_t> nibbles;
    nibbles.reserve(cw_soft.size());
    for (const auto& cw : cw_soft) {
        nibbles.push_back(hamming_decode_soft(cw, cr_app));
    }
    return nibbles;
}

} // namespace host_sim
