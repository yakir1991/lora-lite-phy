#pragma once
#include <vector>
#include <complex>
#include <cstdint>
#include <utility>
#include <span>
#include "lora/workspace.hpp"
#include "lora/utils/hamming.hpp"
#include "lora/constants.hpp"

namespace lora::rx {

std::pair<std::span<uint8_t>, bool> loopback_rx(Workspace& ws,
                                                std::span<const std::complex<float>> samples,
                                                uint32_t sf,
                                                utils::CodeRate cr,
                                                size_t payload_len,
                                                bool check_sync = false,
                                                uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC);

} // namespace lora::rx

