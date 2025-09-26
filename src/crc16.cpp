#include "crc16.hpp"

namespace lora_lite {

uint16_t crc16_ibm(std::span<const uint8_t> data, uint16_t init) {
    uint16_t crc = init;
    for (uint8_t b : data) {
        crc ^= static_cast<uint16_t>(b) << 8;
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x8000) crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            else crc <<= 1;
        }
    }
    return crc & 0xFFFF;
}

} // namespace lora_lite
