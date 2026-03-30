#include "host_sim/lora_replay/header_encoder.hpp"

#include "host_sim/gray.hpp"
#include "host_sim/lora_replay/stage_processing.hpp"

#include <algorithm>
#include <stdexcept>

namespace host_sim::lora_replay
{
namespace
{

int modulo(int value, int mod)
{
    int result = value % mod;
    if (result < 0) {
        result += mod;
    }
    return result;
}

std::vector<bool> uint_to_bits(uint16_t value, int bit_count)
{
    std::vector<bool> bits(bit_count);
    for (int i = 0; i < bit_count; ++i) {
        bits[bit_count - 1 - i] = ((value >> i) & 0x1u) != 0;
    }
    return bits;
}

uint16_t bits_to_uint(const std::vector<bool>& bits)
{
    uint16_t value = 0;
    for (bool bit : bits) {
        value = static_cast<uint16_t>((value << 1) | (bit ? 1u : 0u));
    }
    return value;
}

uint8_t hamming_encode_header(uint8_t nibble)
{
    const bool d3 = ((nibble >> 3) & 0x1) != 0;
    const bool d2 = ((nibble >> 2) & 0x1) != 0;
    const bool d1 = ((nibble >> 1) & 0x1) != 0;
    const bool d0 = (nibble & 0x1) != 0;

    const bool b4 = d3;
    const bool b5 = d2;
    const bool b6 = d1;
    const bool b7 = d0;

    const bool b3 = b7 ^ b6 ^ b5;
    const bool b2 = b6 ^ b5 ^ b4;
    const bool b1 = b7 ^ b6 ^ b4;
    const bool parity = b7 ^ b6 ^ b5 ^ b4 ^ b3 ^ b2 ^ b1;
    const bool b0 = parity;

    uint8_t codeword = 0;
    const bool bits_array[8] = {b7, b6, b5, b4, b3, b2, b1, b0};
    for (bool bit : bits_array) {
        codeword = static_cast<uint8_t>((codeword << 1) | (bit ? 1u : 0u));
    }
    return codeword;
}

} // namespace

uint8_t compute_header_checksum(int payload_len, bool has_crc, int cr)
{
    bool h[12] = {
        static_cast<bool>((payload_len >> 7) & 0x1),
        static_cast<bool>((payload_len >> 6) & 0x1),
        static_cast<bool>((payload_len >> 5) & 0x1),
        static_cast<bool>((payload_len >> 4) & 0x1),
        static_cast<bool>((payload_len >> 3) & 0x1),
        static_cast<bool>((payload_len >> 2) & 0x1),
        static_cast<bool>((payload_len >> 1) & 0x1),
        static_cast<bool>(payload_len & 0x1),
        has_crc,
        static_cast<bool>((cr >> 2) & 0x1),
        static_cast<bool>((cr >> 1) & 0x1),
        static_cast<bool>(cr & 0x1),
    };

    static constexpr int G[5][12] = {
        {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}, // c4
        {1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0}, // c3
        {0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1}, // c2
        {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1}, // c1
        {0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1}, // c0
    };

    int bits[5] = {0, 0, 0, 0, 0};
    for (int row = 0; row < 5; ++row) {
        int acc = 0;
        for (int col = 0; col < 12; ++col) {
            acc ^= (G[row][col] & 0x1) & static_cast<int>(h[col]);
        }
        bits[row] = acc & 0x1;
    }
    return static_cast<uint8_t>((bits[0] << 4) | (bits[1] << 3) | (bits[2] << 2) | (bits[3] << 1) | bits[4]);
}

std::vector<uint8_t> build_header_nibbles(int payload_len, bool has_crc, int cr)
{
    const uint8_t checksum = compute_header_checksum(payload_len, has_crc, cr);
    std::vector<uint8_t> nibbles;
    nibbles.reserve(5);
    nibbles.push_back(static_cast<uint8_t>((payload_len >> 4) & 0xF));
    nibbles.push_back(static_cast<uint8_t>(payload_len & 0xF));
    nibbles.push_back(static_cast<uint8_t>(((cr & 0x7) << 1) | (has_crc ? 0x1 : 0x0)));
    nibbles.push_back(static_cast<uint8_t>((checksum >> 4) & 0x1));
    nibbles.push_back(static_cast<uint8_t>(checksum & 0xF));
    return nibbles;
}

std::vector<uint16_t> encode_header_symbols(int sf,
                                            bool /*ldro*/,
                                            int payload_len,
                                            bool has_crc,
                                            int cr)
{
    if (sf <= 0 || payload_len < 0 || payload_len > 0xFF || cr < 1 || cr > 4) {
        throw std::invalid_argument("Invalid LoRa header parameters");
    }

    const int sf_app = sf - 2;
    if (sf_app <= 0) {
        throw std::invalid_argument("Invalid effective spreading factor");
    }

    constexpr int cw_len = 8;
    const int mask_full = (1 << sf) - 1;
    const int mask_app = (1 << sf_app) - 1;
    const int codeword_rows = sf_app;

    auto nibbles = build_header_nibbles(payload_len, has_crc, cr);
    while (nibbles.size() < static_cast<std::size_t>(codeword_rows)) {
        nibbles.push_back(0);
    }

    std::vector<std::vector<bool>> deinter_matrix(codeword_rows, std::vector<bool>(cw_len, false));
    for (int row = 0; row < codeword_rows; ++row) {
        const uint8_t nibble = nibbles[row] & 0xF;
        const uint8_t codeword = hamming_encode_header(nibble);
        deinter_matrix[row] = uint_to_bits(codeword, cw_len);
    }

    std::vector<std::vector<bool>> inter_matrix(cw_len, std::vector<bool>(sf_app, false));
    for (int col = 0; col < cw_len; ++col) {
        for (int bit = 0; bit < sf_app; ++bit) {
            const int row = modulo(col - bit - 1, sf_app);
            inter_matrix[col][bit] = deinter_matrix[row][col];
        }
    }

    std::vector<uint16_t> symbols(static_cast<std::size_t>(cw_len), 0);
    for (int idx = 0; idx < cw_len; ++idx) {
        const uint16_t value = bits_to_uint(inter_matrix[idx]) & mask_app;
        uint16_t raw = host_sim::gray_decode(value);
        if (sf_app < sf) {
            raw = static_cast<uint16_t>((raw << 2) & mask_full);
        }
        raw = static_cast<uint16_t>((raw + 1u) & mask_full);
        symbols[static_cast<std::size_t>(idx)] = raw;
    }

    return symbols;
}

std::vector<uint16_t> encode_header_symbols(const host_sim::LoRaMetadata& meta,
                                            int payload_len,
                                            bool has_crc,
                                            int cr)
{
    return encode_header_symbols(meta.sf, meta.ldro, payload_len, has_crc, cr);
}

std::vector<uint8_t> build_payload_with_crc(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> buffer = payload;
    const uint16_t crc = compute_lora_crc(payload);
    buffer.push_back(static_cast<uint8_t>(crc & 0xFF));
    buffer.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    return buffer;
}

} // namespace host_sim::lora_replay
