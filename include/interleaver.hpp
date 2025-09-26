#pragma once
#include <cstdint>
#include <vector>

namespace lora_lite {
// Diagonal interleaver for LoRa (per symbol block). sf_app = sf-2 for sf>=7 else sf.
void interleave_bits(const uint8_t* in, uint8_t* out, int sf_app, int cw_len=8);
void deinterleave_bits(const uint8_t* in, uint8_t* out, int sf_app, int cw_len=8);
}
