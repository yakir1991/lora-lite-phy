#pragma once
#include <vector>
#include <complex>
#include <span>
#include <optional>
#include <cstdint>
#include "lora/workspace.hpp"

namespace lora::rx {

// Detects the preamble (using `detect_preamble_os`), estimates and
// compensates CFO/STO (via `estimate_cfo_from_preamble`), aligns to the
// sync word, and returns the corrected samples along with the index of
// the header start. The returned `size_t` is `hdr_start_base`, defined as
// `sync_start + 2*N + N/4` where `N` is the symbol length.
std::optional<std::pair<std::vector<std::complex<float>>, size_t>>
align_and_extract_data(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    size_t min_preamble_syms,
    uint8_t expected_sync);

} // namespace lora::rx
