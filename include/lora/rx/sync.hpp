#pragma once
#include <complex>
#include <vector>
#include <optional>
#include <span>
#include <cstdint>
#include "lora/workspace.hpp"
#include "lora/rx/demod.hpp"

namespace lora::rx {

// Inline wrapper to maintain legacy name
inline uint32_t demod_symbol(Workspace& ws, const std::complex<float>* block) {
    return demod_symbol_peak(ws, block);
}

// Align input samples to the sync word and return CFO/STO-corrected samples
// together with the detected sync start index. On failure returns std::nullopt.
std::optional<std::pair<std::vector<std::complex<float>>, size_t>> align_to_sync(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    size_t min_preamble_syms,
    uint8_t expected_sync);

} // namespace lora::rx
