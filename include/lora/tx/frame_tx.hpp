#pragma once
#include <vector>
#include <complex>
#include <cstdint>
#include <span>
#include "lora/workspace.hpp"
#include "lora/rx/header.hpp"

namespace lora::tx {

// Build a full frame: [header(2)+CRC(2)] + [payload] + [payload CRC(2)],
// whiten the entire frame, then apply Hamming/interleaver/gray and modulate
// to upchirp IQ (no preamble/sync). Returns IQ span.
std::span<const std::complex<float>> frame_tx(Workspace& ws,
                                              const std::vector<uint8_t>& payload,
                                              uint32_t sf,
                                              lora::utils::CodeRate cr,
                                              const lora::rx::LocalHeader& hdr);

} // namespace lora::tx

