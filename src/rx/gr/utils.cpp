#include "lora/rx/gr/utils.hpp"

#include <algorithm>
#include <bit>

namespace lora::rx::gr {

uint32_t gray_encode(uint32_t value) {
    return value ^ (value >> 1);
}

uint32_t gray_decode(uint32_t value) {
    uint32_t res = value;
    while (value >>= 1) {
        res ^= value;
    }
    return res;
}

InterleaverMap make_diagonal_interleaver(uint32_t sf, uint32_t cr_plus4) {
    InterleaverMap map;
    map.n_in = sf * cr_plus4;
    map.n_out = map.n_in;
    map.map.resize(map.n_in);
    uint32_t rows = sf;
    uint32_t cols = cr_plus4;
    for (uint32_t col = 0; col < cols; ++col) {
        for (uint32_t row = 0; row < rows; ++row) {
            uint32_t dst = col * rows + row;

            // GNU Radio diagonal interleaver uses mod((i - j - 1), sf) to select
            // the destination row for a given input column i and row j.
            int rotated = static_cast<int>(col) - static_cast<int>(row) - 1;
            rotated %= static_cast<int>(rows);
            if (rotated < 0) rotated += static_cast<int>(rows);

            uint32_t src = static_cast<uint32_t>(rotated) * cols + col;
            map.map[dst] = src;
        }
    }
    return map;
}

static uint8_t parity(uint32_t v) {
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v &= 0xFu;
    static constexpr uint8_t lut[16] = {0, 1, 1, 0, 1, 0, 0, 1,
                                        1, 0, 0, 1, 0, 1, 1, 0};
    return lut[v];
}

static uint8_t compute_syndrome(uint16_t cw, uint8_t nbits) {
    uint8_t d0 = (cw >> 0) & 1;
    uint8_t d1 = (cw >> 1) & 1;
    uint8_t d2 = (cw >> 2) & 1;
    uint8_t d3 = (cw >> 3) & 1;
    uint8_t p1 = (cw >> 4) & 1;
    uint8_t p2 = (cw >> 5) & 1;
    uint8_t p3 = (cw >> 6) & 1;
    uint8_t p0 = (cw >> 7) & 1;

    uint8_t s1 = (d0 ^ d1 ^ d3) ^ p1;
    uint8_t s2 = (nbits >= 6) ? ((d0 ^ d2 ^ d3) ^ p2) : 0;
    uint8_t s3 = (nbits >= 7) ? ((d1 ^ d2 ^ d3) ^ p3) : 0;
    uint8_t s0 = (nbits == 8) ? parity(cw & 0xFFu) : 0;

    return static_cast<uint8_t>((s0 << 3) | (s3 << 2) | (s2 << 1) | s1);
}

HammingTables make_hamming_tables() {
    HammingTables T;
    T.synd_45.fill(-1);
    T.synd_46.fill(-1);
    T.synd_47.fill(-1);
    T.synd_48.fill(-1);

    for (int d = 0; d < 16; ++d) {
        uint8_t d0 = (d >> 0) & 1;
        uint8_t d1 = (d >> 1) & 1;
        uint8_t d2 = (d >> 2) & 1;
        uint8_t d3 = (d >> 3) & 1;

        uint8_t p1 = d0 ^ d1 ^ d3;
        uint8_t p2 = d0 ^ d2 ^ d3;
        uint8_t p3 = d1 ^ d2 ^ d3;
        uint8_t p0 = d0 ^ d1 ^ d2 ^ d3 ^ p1 ^ p2 ^ p3;

        T.enc_45[d] = static_cast<uint8_t>((d & 0xF) | (p1 << 4));
        T.enc_46[d] = static_cast<uint8_t>((d & 0xF) | (p1 << 4) | (p2 << 5));
        T.enc_47[d] = static_cast<uint8_t>((d & 0xF) | (p1 << 4) | (p2 << 5) | (p3 << 6));
        T.enc_48[d] = static_cast<uint8_t>((d & 0xF) | (p1 << 4) | (p2 << 5) | (p3 << 6) | (p0 << 7));
    }

    auto fill_dec = [](auto& arr, uint8_t nbits) {
        uint16_t base = 0;
        for (int i = 0; i < nbits; ++i) {
            uint16_t cw = base ^ (1u << i);
            uint8_t syn = compute_syndrome(cw, nbits);
            if (syn < arr.size())
                arr[syn] = static_cast<int8_t>(i);
        }
    };

    fill_dec(T.synd_45, 5);
    fill_dec(T.synd_46, 6);
    fill_dec(T.synd_47, 7);
    fill_dec(T.synd_48, 8);

    T.synd_45[0] = -1;
    T.synd_46[0] = -1;
    T.synd_46[3] = -1;

    return T;
}

std::pair<uint16_t, uint8_t> hamming_encode4(uint8_t nibble, CodeRate cr, const HammingTables& tables) {
    nibble &= 0xF;
    switch (cr) {
        case CodeRate::CR45: return {tables.enc_45[nibble], 5};
        case CodeRate::CR46: return {tables.enc_46[nibble], 6};
        case CodeRate::CR47: return {tables.enc_47[nibble], 7};
        case CodeRate::CR48: return {tables.enc_48[nibble], 8};
    }
    return {0, 0};
}

std::optional<std::pair<uint8_t, bool>> hamming_decode4(uint16_t codeword, uint8_t nbits, CodeRate cr, const HammingTables& tables) {
    codeword &= (1u << nbits) - 1u;
    uint8_t syn = compute_syndrome(codeword, nbits);
    uint8_t nibble = static_cast<uint8_t>(codeword & 0xF);

    auto correct_with = [&](const auto& synd_arr) -> std::optional<std::pair<uint8_t, bool>> {
        if (syn == 0) return std::make_pair(nibble, false);
        if (syn >= synd_arr.size()) return std::nullopt;
        int8_t idx = synd_arr[syn];
        if (idx < 0) return std::nullopt;
        codeword ^= (1u << idx);
        nibble = static_cast<uint8_t>(codeword & 0xF);
        return std::make_pair(nibble, true);
    };

    switch (cr) {
        case CodeRate::CR45:
            return correct_with(tables.synd_45);
        case CodeRate::CR46:
            return correct_with(tables.synd_46);
        case CodeRate::CR47:
            return correct_with(tables.synd_47);
        case CodeRate::CR48:
            return correct_with(tables.synd_48);
    }
    return std::nullopt;
}

static uint16_t reflect_bits(uint16_t value, int bits) {
    uint16_t r = 0;
    for (int i = 0; i < bits; ++i)
        if (value & (1u << i)) r |= 1u << (bits - 1 - i);
    return r;
}

static uint8_t reflect_bits8(uint8_t value) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i)
        if (value & (1u << i)) r |= 1u << (7 - i);
    return r;
}

Crc16Ccitt::Crc16Ccitt(uint16_t poly, uint16_t init, uint16_t xorout, bool reflect_in, bool reflect_out)
    : poly_(poly), init_(init), xorout_(xorout), reflect_in_(reflect_in), reflect_out_(reflect_out) {}

uint16_t Crc16Ccitt::compute(const uint8_t* data, size_t len) const {
    uint16_t crc = init_;
    for (size_t i = 0; i < len; ++i) {
        uint8_t newByte = data[i];
        for (unsigned char b = 0; b < 8; ++b) {
            if (((crc & 0x8000) >> 8) ^ (newByte & 0x80)) {
                crc = static_cast<uint16_t>((crc << 1) ^ poly_);
            } else {
                crc = static_cast<uint16_t>(crc << 1);
            }
            newByte <<= 1;
        }
    }
    return crc ^ xorout_;
}

std::pair<uint8_t, uint8_t> Crc16Ccitt::make_trailer_be(const uint8_t* data, size_t len) const {
    uint16_t crc = compute(data, len);
    return {static_cast<uint8_t>((crc >> 8) & 0xFFu), static_cast<uint8_t>(crc & 0xFFu)};
}

std::pair<uint8_t, uint8_t> Crc16Ccitt::make_trailer_le(const uint8_t* data, size_t len) const {
    uint16_t crc = compute(data, len);
    return {static_cast<uint8_t>(crc & 0xFFu), static_cast<uint8_t>((crc >> 8) & 0xFFu)};
}

LfsrWhitening LfsrWhitening::pn9_default() {
    return LfsrWhitening(0xFF);
}

uint8_t LfsrWhitening::step() {
    uint8_t b0 = (state_ >> 0) & 1u;
    uint8_t b1 = (state_ >> 1) & 1u;
    uint8_t b2 = (state_ >> 2) & 1u;
    uint8_t b5 = (state_ >> 5) & 1u;
    uint8_t next = static_cast<uint8_t>(b5 ^ b2 ^ b1 ^ b0);
    state_ = static_cast<uint8_t>(((state_ << 1) | next) & 0xFFu);
    return state_;
}

void LfsrWhitening::apply(uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    for (size_t i = 0; i < len; ++i) {
        uint8_t prn = step();
        data[i] ^= prn;
    }
}

namespace {
constexpr std::array<uint8_t, kWhiteningSeqLen> kWhiteningSeq = {
    0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE1, 0xC2, 0x85, 0x0B, 0x17, 0x2F, 0x5E, 0xBC, 0x78, 0xF1, 0xE3,
    0xC6, 0x8D, 0x1A, 0x34, 0x68, 0xD0, 0xA0, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x11, 0x23, 0x47,
    0x8E, 0x1C, 0x38, 0x71, 0xE2, 0xC4, 0x89, 0x12, 0x25, 0x4B, 0x97, 0x2E, 0x5C, 0xB8, 0x70, 0xE0,
    0xC0, 0x81, 0x03, 0x06, 0x0C, 0x19, 0x32, 0x64, 0xC9, 0x92, 0x24, 0x49, 0x93, 0x26, 0x4D, 0x9B,
    0x37, 0x6E, 0xDC, 0xB9, 0x72, 0xE4, 0xC8, 0x90, 0x20, 0x41, 0x82, 0x05, 0x0A, 0x15, 0x2B, 0x56,
    0xAD, 0x5B, 0xB6, 0x6D, 0xDA, 0xB5, 0x6B, 0xD6, 0xAC, 0x59, 0xB2, 0x65, 0xCB, 0x96, 0x2C, 0x58,
    0xB0, 0x61, 0xC3, 0x87, 0x0F, 0x1F, 0x3E, 0x7D, 0xFB, 0xF6, 0xED, 0xDB, 0xB7, 0x6F, 0xDE, 0xBD,
    0x7A, 0xF5, 0xEB, 0xD7, 0xAE, 0x5D, 0xBA, 0x74, 0xE8, 0xD1, 0xA2, 0x44, 0x88, 0x10, 0x21, 0x43,
    0x86, 0x0D, 0x1B, 0x36, 0x6C, 0xD8, 0xB1, 0x63, 0xC7, 0x8F, 0x1E, 0x3C, 0x79, 0xF3, 0xE7, 0xCE,
    0x9C, 0x39, 0x73, 0xE6, 0xCC, 0x98, 0x31, 0x62, 0xC5, 0x8B, 0x16, 0x2D, 0x5A, 0xB4, 0x69, 0xD2,
    0xA4, 0x48, 0x91, 0x22, 0x45, 0x8A, 0x14, 0x29, 0x52, 0xA5, 0x4A, 0x95, 0x2A, 0x54, 0xA9, 0x53,
    0xA7, 0x4E, 0x9D, 0x3B, 0x77, 0xEE, 0xDD, 0xBB, 0x76, 0xEC, 0xD9, 0xB3, 0x67, 0xCF, 0x9E, 0x3D,
    0x7B, 0xF7, 0xEF, 0xDF, 0xBF, 0x7E, 0xFD, 0xFA, 0xF4, 0xE9, 0xD3, 0xA6, 0x4C, 0x99, 0x33, 0x66,
    0xCD, 0x9A, 0x35, 0x6A, 0xD4, 0xA8, 0x51, 0xA3, 0x46, 0x8C, 0x18, 0x30, 0x60, 0xC1, 0x83, 0x07,
    0x0E, 0x1D, 0x3A, 0x75, 0xEA, 0xD5, 0xAA, 0x55, 0xAB, 0x57, 0xAF, 0x5F, 0xBE, 0x7C, 0xF9, 0xF2,
    0xE5, 0xCA, 0x94, 0x28, 0x50, 0xA1, 0x42, 0x84, 0x09, 0x13, 0x27, 0x4F, 0x9F, 0x3F, 0x7F
};
} // namespace

const std::array<uint8_t, kWhiteningSeqLen>& whitening_sequence() {
    return kWhiteningSeq;
}

void dewhiten_payload(std::span<uint8_t> data, size_t offset) {
    if (offset >= kWhiteningSeqLen) offset %= kWhiteningSeqLen;
    for (size_t i = 0; i < data.size(); ++i) {
        size_t idx = (offset + i) % kWhiteningSeqLen;
        data[i] ^= kWhiteningSeq[idx];
    }
}

} // namespace lora::rx::gr
