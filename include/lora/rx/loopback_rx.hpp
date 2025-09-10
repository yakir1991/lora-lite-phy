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

// Header-based decode integrated with loopback path.
// Expects preamble + sync then a frame layout:
//   [header(2)+CRC(2)] + [payload bytes] + [payload CRC(2)]
// Whitening is applied to the entire frame in TX.
// If os_aware=true, performs OS detection and polyphase decimation before CFO/STO compensation and decode.
std::pair<std::span<uint8_t>, bool> loopback_rx_header(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms = 8,
    bool os_aware = true);

// Auto-length variant: derives payload length from header.
std::pair<std::span<uint8_t>, bool> loopback_rx_header_auto(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    utils::CodeRate cr,
    size_t min_preamble_syms = 8,
    bool os_aware = true);

// Auto-length variant with explicit sync-word selection (default 0x34 public).
std::pair<std::span<uint8_t>, bool> loopback_rx_header_auto_sync(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    utils::CodeRate cr,
    size_t min_preamble_syms = 8,
    bool os_aware = true,
    uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC);

} // namespace lora::rx
