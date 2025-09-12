#pragma once
#include <optional>
#include <span>
#include <complex>
#include <cstdint>
#include <vector>
#include <utility>
#include "lora/workspace.hpp"
#include "lora/rx/header.hpp"

namespace lora::rx {

// Internal implementation extracted from frame.cpp for maintainability.
// Performs OS-aware align + CFO/STO + GR-style header mapping and parsing.
std::optional<LocalHeader> decode_header_with_preamble_cfo_sto_os_impl(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms = 8,
    uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC);

// Decode header directly from symbol-aligned data using GNU Radio mapping.
// Returns the parsed LocalHeader (if any) and the decoded nibble stream.
std::pair<std::optional<LocalHeader>, std::vector<uint8_t>> decode_header_from_symbols(
    Workspace& ws,
    std::span<const std::complex<float>> data,
    uint32_t sf);

} // namespace lora::rx

