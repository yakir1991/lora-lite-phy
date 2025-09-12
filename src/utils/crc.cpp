#include "lora/utils/crc.hpp"

namespace lora::utils {

static inline uint16_t reflect16(uint16_t x) {
    x = (x >> 8) | (x << 8);
    x = ((x & 0xF0F0u) >> 4) | ((x & 0x0F0Fu) << 4);
    x = ((x & 0xCCCCu) >> 2) | ((x & 0x3333u) << 2);
    x = ((x & 0xAAAAu) >> 1) | ((x & 0x5555u) << 1);
    return x;
}

uint16_t Crc16Ccitt::compute(const uint8_t* data, size_t len) const {
    uint16_t crc = init;
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        if (ref_in) {
            byte = (uint8_t)((byte * 0x0202020202ULL & 0x010884422010ULL) % 1023);
        }
        crc ^= (uint16_t)byte << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000u) crc = (crc << 1) ^ poly;
            else               crc = (crc << 1);
        }
    }
    if (ref_out) crc = reflect16(crc);
    crc ^= xorout;
    return crc;
}

std::pair<bool, uint16_t> Crc16Ccitt::verify_with_trailer_be(const uint8_t* data, size_t len) const {
    if (len < 2) return {false, 0};
    uint16_t calc = compute(data, len - 2);
    uint16_t got  = (uint16_t(data[len-2]) << 8) | uint16_t(data[len-1]);
    return {calc == got, calc};
}

std::pair<uint8_t,uint8_t> Crc16Ccitt::make_trailer_be(const uint8_t* data, size_t len) const {
    uint16_t c = compute(data, len);
    return { uint8_t((c >> 8) & 0xFF), uint8_t(c & 0xFF) };
}

std::pair<bool, uint16_t> Crc16Ccitt::verify_with_trailer_le(const uint8_t* data, size_t len) const {
    if (len < 2) return {false, 0};
    uint16_t calc = compute(data, len - 2);
    uint16_t got  = uint16_t(data[len-2]) | (uint16_t(data[len-1]) << 8);
    return {calc == got, calc};
}

std::pair<uint8_t,uint8_t> Crc16Ccitt::make_trailer_le(const uint8_t* data, size_t len) const {
    uint16_t c = compute(data, len);
    return { uint8_t(c & 0xFF), uint8_t((c >> 8) & 0xFF) };
}

} // namespace lora::utils
