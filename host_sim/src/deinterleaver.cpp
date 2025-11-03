#include "host_sim/deinterleaver.hpp"

#include "host_sim/gray.hpp"

#include <stdexcept>

namespace host_sim
{

namespace
{

std::vector<bool> int_to_bits(uint16_t value, int bit_count)
{
    std::vector<bool> bits(bit_count);
    for (int i = 0; i < bit_count; ++i) {
        bits[bit_count - 1 - i] = (value >> i) & 0x1u;
    }
    return bits;
}

uint8_t bits_to_uint8(const std::vector<bool>& bits)
{
    uint8_t result = 0;
    for (bool bit : bits) {
        result = static_cast<uint8_t>((result << 1) | (bit ? 1 : 0));
    }
    return result;
}

inline int modulo(int a, int m)
{
    int result = a % m;
    if (result < 0) {
        result += m;
    }
    return result;
}

}

std::vector<uint8_t> deinterleave(const std::vector<uint16_t>& symbols, const DeinterleaverConfig& cfg, std::size_t& consumed)
{
    const bool use_ldro = cfg.ldro || cfg.is_header;
    const int sf_app = use_ldro ? cfg.sf - 2 : cfg.sf;
    const int cw_len = cfg.is_header ? 8 : cfg.cr + 4;
    if (static_cast<int>(symbols.size()) < cw_len) {
        throw std::runtime_error("Not enough symbols to deinterleave block");
    }

    std::vector<std::vector<bool>> inter_matrix(cw_len);
    const uint16_t mask_full = static_cast<uint16_t>((1u << cfg.sf) - 1u);
    const uint16_t mask_app = static_cast<uint16_t>((1u << sf_app) - 1u);
    for (int i = 0; i < cw_len; ++i) {
        uint16_t raw = static_cast<uint16_t>(symbols[i] & mask_full);
        raw = static_cast<uint16_t>((raw - 1u) & mask_full);
        if (use_ldro) {
            raw = static_cast<uint16_t>(raw >> 2);
        }
        const uint16_t gray_input = raw;
        const uint16_t mapped = static_cast<uint16_t>(gray_input ^ (gray_input >> 1));
        inter_matrix[i] = int_to_bits(mapped & mask_app, sf_app);
    }

    std::vector<std::vector<bool>> deinter_matrix(sf_app, std::vector<bool>(cw_len));
    for (int i = 0; i < cw_len; ++i) {
        for (int j = 0; j < sf_app; ++j) {
            const int row = modulo(i - j - 1, sf_app);
            deinter_matrix[row][i] = inter_matrix[i][j];
        }
    }

    std::vector<uint8_t> result(sf_app);
    for (int i = 0; i < sf_app; ++i) {
        result[i] = bits_to_uint8(deinter_matrix[i]);
    }

    consumed = cw_len;
    return result;
}

} // namespace host_sim
