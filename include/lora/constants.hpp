#pragma once
#include <cstdint>

namespace lora {

// Default LoRa sync words as defined by the Semtech specification.
// 0x34 is used for public networks, while 0x12 selects private networks.
inline constexpr uint8_t LORA_SYNC_WORD_PUBLIC  = 0x34;
inline constexpr uint8_t LORA_SYNC_WORD_PRIVATE = 0x12;

} // namespace lora

