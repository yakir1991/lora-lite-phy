#pragma once
#include <vector>
#include <complex>
#include <cstdint>
#include <span>
#include <utility>
#include <optional>
#include "lora/workspace.hpp"
#include "lora/rx/preamble.hpp"
#include "lora/rx/header.hpp"

namespace lora::rx {

// Decode a full frame with preamble + sync; expects frame layout:
// [header(2)+CRC(2)] + [payload bytes] + [payload CRC(2)]
// Whitening applied to the entire frame in TX.
std::pair<std::span<uint8_t>, bool> decode_frame_with_preamble(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms = 8,
    uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC);

// Auto-length version: detect header first to derive payload length, then decode.
std::pair<std::span<uint8_t>, bool> decode_frame_with_preamble_cfo_sto_os_auto(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms = 8,
    uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC);

// Decode full frame with CFO and integer STO estimation/compensation.
std::pair<std::span<uint8_t>, bool> decode_frame_with_preamble_cfo_sto(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms = 8,
    uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC);

// OS-aware version (tries OS candidates and phases, decimates, then runs CFO/STO flow).
std::pair<std::span<uint8_t>, bool> decode_frame_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms = 8,
    uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC);

// Decode only the header (LEN/CR/CRC) and return it if parsed.
std::optional<LocalHeader> decode_header_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms = 8,
    uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC);

// Decode payload bytes without enforcing payload CRC (useful for diagnostics).
// Returns (payload, header_ok). If header_ok=false, the payload length might be unreliable.
std::pair<std::vector<uint8_t>, bool> decode_payload_no_crc_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms = 8,
    uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC);

} // namespace lora::rx
