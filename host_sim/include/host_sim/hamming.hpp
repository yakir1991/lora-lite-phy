#pragma once

#include <cstdint>
#include <vector>

namespace host_sim
{

uint8_t hamming_decode(uint8_t codeword, int cr_app);

std::vector<uint8_t> hamming_decode_block(const std::vector<uint8_t>& codewords, bool header, int cr);

} // namespace host_sim
