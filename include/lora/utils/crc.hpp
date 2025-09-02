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
};

} // namespace lora::utils
