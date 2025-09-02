#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <optional>
#include <utility>

namespace lora::utils {

enum class CodeRate : uint8_t { CR45 = 1, CR46 = 2, CR47 = 3, CR48 = 4 };

struct HammingTables {
    std::array<uint8_t, 16> enc_45{}; // 5-bit packed
    std::array<uint8_t, 16> enc_46{}; // 6-bit packed
    std::array<uint8_t, 16> enc_47{}; // 7-bit packed
    std::array<uint8_t, 16> enc_48{}; // 8-bit packed
};

HammingTables make_placeholder_tables();

std::pair<uint16_t, uint8_t> hamming_encode4(uint8_t nibble, CodeRate cr, const HammingTables& T);

std::optional<std::pair<uint8_t, bool>> hamming_decode4(uint16_t codeword, uint8_t nbits, CodeRate cr, const HammingTables& T);

} // namespace lora::utils
