#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "lora/rx/lite/lora_primitives.hpp"
#include "lora/rx/lite/lora_utils.hpp"

namespace lora::rx::gr {

struct LocalHeader {
    uint8_t payload_len{};
    CodeRate cr{CodeRate::CR45};
    bool has_crc{true};
};

std::optional<LocalHeader> parse_standard_lora_header(const uint8_t* hdr, size_t len);

std::optional<LocalHeader> decode_header_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    CodeRate cr,
    size_t min_preamble_syms,
    uint8_t expected_sync_word,
    std::vector<uint32_t>* out_raw_bins = nullptr,
    std::vector<uint8_t>* out_nibbles = nullptr);

} // namespace lora::rx::gr

