#pragma once
#include <cstdint>
#include <cstddef>
#include <utility>

namespace lora::utils {


struct Crc16Ccitt {
    uint16_t poly   = 0x1021;
    uint16_t init   = 0xFFFF;
    uint16_t xorout = 0x0000;
    bool ref_in     = false;
    bool ref_out    = false;


    uint16_t compute(const uint8_t* data, size_t len) const;


    std::pair<bool, uint16_t> verify_with_trailer_be(const uint8_t* data, size_t len) const;


    std::pair<uint8_t,uint8_t> make_trailer_be(const uint8_t* data, size_t len) const;


    std::pair<bool, uint16_t> verify_with_trailer_le(const uint8_t* data, size_t len) const;


    std::pair<uint8_t,uint8_t> make_trailer_le(const uint8_t* data, size_t len) const;
};

// Compute the 5-bit checksum used by the explicit LoRa header.
// Inputs `n0`, `n1`, `n2` are the low nibbles of the length high byte,
// length low byte and flags nibble respectively. The return value packs
// bits c4..c0 into the low five bits of the result.
inline uint8_t crc_header(uint8_t n0, uint8_t n1, uint8_t n2) {
    bool c4 = ((n0 & 0b1000) >> 3) ^ ((n0 & 0b0100) >> 2) ^ ((n0 & 0b0010) >> 1) ^ (n0 & 0b0001);
    bool c3 = ((n0 & 0b1000) >> 3) ^ ((n1 & 0b1000) >> 3) ^ ((n1 & 0b0100) >> 2) ^ ((n1 & 0b0010) >> 1) ^ (n2 & 0x1);
    bool c2 = ((n0 & 0b0100) >> 2) ^ ((n1 & 0b1000) >> 3) ^ (n1 & 0x1) ^ ((n2 & 0b1000) >> 3) ^ ((n2 & 0b0010) >> 1);
    bool c1 = ((n0 & 0b0010) >> 1) ^ ((n1 & 0b0100) >> 2) ^ (n1 & 0x1) ^ ((n2 & 0b0100) >> 2) ^ ((n2 & 0b0010) >> 1) ^ (n2 & 0x1);
    bool c0 = (n0 & 0x1) ^ ((n1 & 0b0010) >> 1) ^ ((n2 & 0b1000) >> 3) ^ ((n2 & 0b0100) >> 2) ^ ((n2 & 0b0010) >> 1) ^ (n2 & 0x1);
    return static_cast<uint8_t>((c4 ? 0x10 : 0x00) |
                                (c3 ? 0x08 : 0x00) |
                                (c2 ? 0x04 : 0x00) |
                                (c1 ? 0x02 : 0x00) |
                                (c0 ? 0x01 : 0x00));
}

} // namespace lora::utils
