#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace lora_lite {
// LoRa payload CRC: CRC-16-IBM (poly 0x1021) init 0x0000, no reflect, no xorout
uint16_t crc16_ibm(std::span<const uint8_t> data, uint16_t init = 0x0000);
}
