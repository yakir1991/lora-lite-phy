#pragma once

#include <vector>

namespace lora::hamming {

// Decode a systematic (4 + r, 4) Hamming-like LoRa codeword in-place.
// Parameters:
//  - codeword: a vector<int> of bits (0/1) representing a single codeword.
//              Expected size is 4 + r where r = parity_bits and r in [1..4].
//  - parity_bits: number of parity bits (CR), 1..4. Maps to LoRa CR=1..4.
// Behavior:
//  - Computes the syndrome using the appropriate parity-check matrix for r.
//  - Corrects a single-bit error if the syndrome matches a valid position.
//  - Leaves the vector modified in-place if a correction occurs; otherwise unchanged.
// Returns:
//  - true if the codeword was valid or corrected successfully.
//  - false if the size is unexpected or the error is uncorrectable.
// Notes:
//  - This routine assumes bits are ordered consistently with the encoder
//    used elsewhere in the pipeline (systematic form with parity appended).
//  - Multi-bit errors are not correctable and will return false.
[[nodiscard]] bool decode_codeword(std::vector<int> &codeword, int parity_bits);

} // namespace lora::hamming

