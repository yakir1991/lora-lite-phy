#include "hamming.hpp"

#include <array>
#include <cstddef>
#include <utility>

namespace lora::hamming {

namespace {

// This file implements a small, in-place Hamming-style decoder used by LoRa.
// In LoRa, each 4-bit nibble is protected by CR extra parity bits where CR ∈ {1,2,3,4},
// yielding codeword lengths of 5, 6, 7, or 8 bits respectively (LoRa coding rates 4/5..4/8).
// We provide parity-check matrices H for each CR that allow:
//  - Computing the syndrome s = H * codeword (mod 2)
//  - Correcting a single-bit error if the syndrome matches one of H's columns
// Behavior:
//  - codeword is modified in place; we only flip one bit at most to correct errors.
//  - We do not remove parity bits; callers who need only the data nibble should strip them externally.
//  - Returns true if the final syndrome is all zeros (no error or successfully corrected one error).
//  - Returns false if the size doesn't match, parity_bits unsupported, or correction failed
//    (e.g., double-bit error leading to a non-matching syndrome).

// Generic single-codeword corrector given a parity-check matrix H (Rows x Cols).
// - Sanitizes bits to binary {0,1}
// - Computes syndrome; if zero, accept
// - Else finds a column equal to syndrome and flips that bit, then re-checks
// - Complexity: O(R*C) per attempt, suitable for tiny LoRa codewords
// Inputs:
//   H        — parity-check matrix with entries 0/1
//   codeword — vector<int> of length Cols, modified in place
// Output:
//   bool — true if codeword passes parity after optional single-bit fix
template <std::size_t Rows, std::size_t Cols>
[[nodiscard]] bool correct_codeword(const std::array<std::array<int, Cols>, Rows> &H,
                                    std::vector<int> &codeword) {
    if (codeword.size() != Cols) {
        return false;
    }

    // Ensure samples are binary
    for (auto &bit : codeword) {
        bit &= 1;
    }

    // syndrome = H * codeword (mod 2)
    std::array<int, Rows> syndrome{};
    for (std::size_t row = 0; row < Rows; ++row) {
        int acc = 0;
        for (std::size_t col = 0; col < Cols; ++col) {
            acc ^= (H[row][col] & codeword[col]);
        }
        syndrome[row] = acc & 1;
    }

    // If syndrome is zero, either no error or an even-number-of-errors that canceled (unlikely here)
    bool zero = true;
    for (auto value : syndrome) {
        if (value != 0) {
            zero = false;
            break;
        }
    }
    if (zero) {
        return true;
    }

    // Single-error correction: find a column in H equal to the syndrome and flip that bit.
    for (std::size_t col = 0; col < Cols; ++col) {
        bool matches = true;
        for (std::size_t row = 0; row < Rows; ++row) {
            if (H[row][col] != syndrome[row]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            codeword[col] ^= 1; // flip erroneous bit

            // Recompute to verify correction.
            for (auto &value : syndrome) {
                value = 0;
            }
            for (std::size_t row = 0; row < Rows; ++row) {
                int acc = 0;
                for (std::size_t col2 = 0; col2 < Cols; ++col2) {
                    acc ^= (H[row][col2] & codeword[col2]);
                }
                syndrome[row] = acc & 1;
            }
            for (auto value : syndrome) {
                if (value != 0) {
                    return false; // unexpected: correction did not clean syndrome
                }
            }
            return true;
        }
    }

    // No matching column -> likely multi-bit error or unsupported pattern
    return false;
}

// Dispatch the appropriate parity-check matrix by number of parity bits (CR).
// Each H has Rows=parity_bits and Cols=(4 + parity_bits) matching LoRa's 4 data bits + CR parity bits.
[[nodiscard]] bool decode_with_parity_bits(std::vector<int> &codeword, int parity_bits) {
    switch (parity_bits) {
    case 1: {
        // CR=1 -> length 5: H is 1x5. Corrects single-bit errors; detects any odd parity violation.
        static constexpr std::array<std::array<int, 5>, 1> H{{
            {1, 1, 1, 1, 1},
        }};
        return correct_codeword(H, codeword);
    }
    case 2: {
        // CR=2 -> length 6: H is 2x6
        static constexpr std::array<std::array<int, 6>, 2> H{{
            {1, 1, 1, 0, 1, 0},
            {0, 1, 1, 1, 0, 1},
        }};
        return correct_codeword(H, codeword);
    }
    case 3: {
        // CR=3 -> length 7: H is 3x7 (classic Hamming(7,4) style)
        static constexpr std::array<std::array<int, 7>, 3> H{{
            {1, 1, 1, 0, 1, 0, 0},
            {0, 1, 1, 1, 0, 1, 0},
            {1, 1, 0, 1, 0, 0, 1},
        }};
        return correct_codeword(H, codeword);
    }
    case 4: {
        // CR=4 -> length 8: H is 4x8 (LoRa's 4/8 rate extension)
        static constexpr std::array<std::array<int, 8>, 4> H{{
            {1, 1, 1, 0, 1, 0, 0, 0},
            {0, 1, 1, 1, 0, 1, 0, 0},
            {1, 1, 0, 1, 0, 0, 1, 0},
            {1, 0, 1, 1, 0, 0, 0, 1},
        }};
        return correct_codeword(H, codeword);
    }
    default:
        return false; // unsupported CR
    }
}

} // namespace

// Public API: correct a single LoRa codeword in place for the given number of parity bits (CR).
// - Expects codeword.size() == 4 + parity_bits and bits in any integer form.
// - Returns true if the codeword is valid after optional single-bit correction.
// - Caller remains responsible for extracting the 4 data bits if needed.
bool decode_codeword(std::vector<int> &codeword, int parity_bits) {
    if (parity_bits <= 0) {
        return false;
    }
    return decode_with_parity_bits(codeword, parity_bits);
}

} // namespace lora::hamming

