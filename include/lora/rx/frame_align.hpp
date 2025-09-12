#pragma once
#include <vector>
#include <complex>
#include <span>
#include <optional>
#include <cstdint>
#include "lora/workspace.hpp"

namespace lora::rx {

// Detects preamble, corrects CFO/STO, aligns to sync, and returns samples
// starting at the header.
std::optional<std::pair<std::vector<std::complex<float>>, size_t>>
align_and_extract_data(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    size_t min_preamble_syms,
    uint8_t expected_sync);

} // namespace lora::rx
