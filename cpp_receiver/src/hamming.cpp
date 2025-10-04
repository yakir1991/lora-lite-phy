#include "hamming.hpp"

#include <array>
#include <cstddef>
#include <utility>

namespace lora::hamming {

namespace {

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

    std::array<int, Rows> syndrome{};
    for (std::size_t row = 0; row < Rows; ++row) {
        int acc = 0;
        for (std::size_t col = 0; col < Cols; ++col) {
            acc ^= (H[row][col] & codeword[col]);
        }
        syndrome[row] = acc & 1;
    }

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

    // Try to locate the column whose pattern matches the syndrome.
    for (std::size_t col = 0; col < Cols; ++col) {
        bool matches = true;
        for (std::size_t row = 0; row < Rows; ++row) {
            if (H[row][col] != syndrome[row]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            codeword[col] ^= 1;

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
                    return false;
                }
            }
            return true;
        }
    }

    return false;
}

[[nodiscard]] bool decode_with_parity_bits(std::vector<int> &codeword, int parity_bits) {
    switch (parity_bits) {
    case 1: {
        static constexpr std::array<std::array<int, 5>, 1> H{{
            {1, 1, 1, 1, 1},
        }};
        return correct_codeword(H, codeword);
    }
    case 2: {
        static constexpr std::array<std::array<int, 6>, 2> H{{
            {1, 1, 1, 0, 1, 0},
            {0, 1, 1, 1, 0, 1},
        }};
        return correct_codeword(H, codeword);
    }
    case 3: {
        static constexpr std::array<std::array<int, 7>, 3> H{{
            {1, 1, 1, 0, 1, 0, 0},
            {0, 1, 1, 1, 0, 1, 0},
            {1, 1, 0, 1, 0, 0, 1},
        }};
        return correct_codeword(H, codeword);
    }
    case 4: {
        static constexpr std::array<std::array<int, 8>, 4> H{{
            {1, 1, 1, 0, 1, 0, 0, 0},
            {0, 1, 1, 1, 0, 1, 0, 0},
            {1, 1, 0, 1, 0, 0, 1, 0},
            {1, 0, 1, 1, 0, 0, 0, 1},
        }};
        return correct_codeword(H, codeword);
    }
    default:
        return false;
    }
}

} // namespace

bool decode_codeword(std::vector<int> &codeword, int parity_bits) {
    if (parity_bits <= 0) {
        return false;
    }
    return decode_with_parity_bits(codeword, parity_bits);
}

} // namespace lora::hamming

