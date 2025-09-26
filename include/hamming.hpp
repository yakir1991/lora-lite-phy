#pragma once
#include <cstdint>
#include <array>
#include <optional>

namespace lora_lite {

enum class CodeRate { CR45=5, CR46=6, CR47=7, CR48=8 };

struct HammingTables {
    std::array<uint8_t,16> enc5{}; // 5,4
    std::array<uint8_t,16> enc6{}; // 6,4
    std::array<uint8_t,16> enc7{}; // 7,4
    std::array<uint8_t,16> enc8{}; // 8,4
};

HammingTables build_hamming_tables();
uint8_t hamming_encode4(uint8_t nibble, CodeRate cr, const HammingTables& t);
std::optional<uint8_t> hamming_decode4(uint8_t code, CodeRate cr, const HammingTables& t, bool allow_correct=true);

}
