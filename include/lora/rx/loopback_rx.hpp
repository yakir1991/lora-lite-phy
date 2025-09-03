#pragma once
#include <vector>
#include <complex>
#include <cstdint>
#include <utility>
#include <span>
#include "lora/workspace.hpp"
#include "lora/utils/hamming.hpp"

namespace lora::rx {

std::pair<std::vector<uint8_t>, bool> loopback_rx(Workspace& ws,
                                                  std::span<const std::complex<float>> samples,
                                                  uint32_t sf,
                                                  utils::CodeRate cr,
                                                  size_t payload_len);

} // namespace lora::rx

