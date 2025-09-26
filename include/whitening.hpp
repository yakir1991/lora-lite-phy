#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lora_lite {

// LoRa whitening uses 4-bit LFSR (x^4 + x^3 + x^2 + 1) with seed 0x01 per spec.
// Generates whitening sequence length bytes (each byte whitened bitwise LSB-first).
void whiten_in_place(uint8_t* data, size_t len, uint8_t seed = 0x01);
void dewhiten_in_place(uint8_t* data, size_t len, uint8_t seed = 0x01);
std::vector<uint8_t> whiten(const std::vector<uint8_t>& in, uint8_t seed = 0x01);
std::vector<uint8_t> dewhiten(const std::vector<uint8_t>& in, uint8_t seed = 0x01);

// Apply whitening sequence to the first prefix_len bytes (XOR with reference table) leaving remaining bytes untouched.
std::vector<uint8_t> dewhiten_prefix(const std::vector<uint8_t>& in, size_t prefix_len);

} // namespace lora_lite
