#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <optional>
#include <utility>

namespace lora::utils {

// LoRa uses a systematic (4, n) block code derived from the classic
// Hamming (7,4) with an additional overall parity bit for n = 8.  The
// coding rate CR = 4/n selects how many parity bits are appended after
// the four data bits of a nibble.
//   * CR45 and CR46 provide single-bit error *detection*.
//   * CR47 corrects any single-bit error within the 7-bit codeword.
//   * CR48 corrects any single-bit error and detects any double-bit error.

enum class CodeRate : uint8_t { CR45 = 1, CR46 = 2, CR47 = 3, CR48 = 4 };

struct HammingTables {
    std::array<uint8_t, 16> enc_45{}; // 5-bit packed
    std::array<uint8_t, 16> enc_46{}; // 6-bit packed
    std::array<uint8_t, 16> enc_47{}; // 7-bit packed
    std::array<uint8_t, 16> enc_48{}; // 8-bit packed

    // Syndrome lookup tables.  Index by the syndrome value to obtain the
    // bit position that should be flipped to correct a single-bit error.
    // Entries are -1 for invalid/unmapped syndromes.
    std::array<int8_t, 2>  synd_45{}; // unused (detect only)
    std::array<int8_t, 4>  synd_46{}; // unused (detect only)
    std::array<int8_t, 8>  synd_47{};
    std::array<int8_t, 16> synd_48{};
};

HammingTables make_hamming_tables();

// Encode a 4-bit nibble according to the selected coding rate.  Returns the
// packed codeword and the number of valid bits in it.
std::pair<uint16_t, uint8_t> hamming_encode4(uint8_t nibble, CodeRate cr, const HammingTables& T);

// Decode a packed codeword produced by ::hamming_encode4().  On success
// returns the recovered nibble and whether a single-bit error was corrected.
// For CR45 and CR46 a non-zero syndrome results in std::nullopt.  For CR47
// a single-bit error is corrected.  For CR48 single-bit errors are corrected
// and any double-bit error results in std::nullopt.
std::optional<std::pair<uint8_t, bool>> hamming_decode4(uint16_t codeword, uint8_t nbits, CodeRate cr, const HammingTables& T);

} // namespace lora::utils
