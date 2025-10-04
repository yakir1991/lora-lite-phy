#pragma once

#include <vector>

namespace lora::hamming {

// Decode a systematic (4 + r, 4) Hamming codeword in-place.
// Returns true if the codeword was valid or successfully corrected.
// Returns false if an uncorrectable error is detected (syndrome does not
// match any single-bit error pattern or the codeword size is unexpected).
[[nodiscard]] bool decode_codeword(std::vector<int> &codeword, int parity_bits);

} // namespace lora::hamming

