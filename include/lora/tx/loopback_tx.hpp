#pragma once
#include <vector>
#include <complex>
#include <cstdint>
#include "lora/workspace.hpp"
#include "lora/utils/hamming.hpp"

namespace lora::tx {

std::vector<std::complex<float>> loopback_tx(Workspace& ws,
                                             const std::vector<uint8_t>& payload,
                                             uint32_t sf,
                                             utils::CodeRate cr);

} // namespace lora::tx

