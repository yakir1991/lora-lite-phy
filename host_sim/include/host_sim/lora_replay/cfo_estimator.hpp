#pragma once

#include "host_sim/deinterleaver.hpp"
#include "host_sim/hamming.hpp"
#include "host_sim/lora_params.hpp"
#include "host_sim/lora_replay/stage_processing.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace host_sim::lora_replay
{

HeaderDecodeResult try_decode_header(const std::vector<uint16_t>& symbols,
                                     std::size_t start,
                                     const host_sim::LoRaMetadata& meta);

std::optional<uint16_t> detect_integer_cfo(const std::vector<uint16_t>& symbols,
                                           const std::vector<uint16_t>& reference,
                                           int sf);

struct PreambleCfoEstimate
{
    uint16_t offset{0};
    std::size_t window{0};
    std::size_t hits{0};
};

struct SyncDetectionResult
{
    std::size_t sync_index{0};
    uint16_t sync_high{0};
    uint16_t sync_low{0};
};

std::optional<SyncDetectionResult> detect_sync_pair(const std::vector<uint16_t>& symbols,
                                                    const host_sim::LoRaMetadata& meta,
                                                    std::size_t max_search = 512,
                                                    std::size_t min_zero_run = 0);

std::optional<PreambleCfoEstimate> infer_integer_cfo_from_preamble(const std::vector<uint16_t>& symbols,
                                                                   const host_sim::LoRaMetadata& meta,
                                                                   std::size_t max_window = 64);

void apply_integer_offset(std::vector<uint16_t>& values, uint16_t offset, int sf);

std::optional<uint16_t> find_header_based_integer_cfo(const std::vector<uint16_t>& symbols,
                                                      const host_sim::LoRaMetadata& meta,
                                                      int search_radius);

struct BruteForceCfoResult
{
    uint16_t offset{0};
    std::size_t header_start{0};
    int score{0};
};

std::optional<BruteForceCfoResult> brute_force_integer_cfo(const std::vector<uint16_t>& symbols,
                                                           const host_sim::LoRaMetadata& meta,
                                                           int search_radius,
                                                           int max_start,
                                                           int min_score,
                                                           std::optional<std::size_t> forced_start = std::nullopt);

std::optional<uint16_t> brute_force_integer_cfo(const std::vector<uint16_t>& symbols,
                                                const host_sim::LoRaMetadata& meta,
                                                int max_offset,
                                                int max_start);

} // namespace host_sim::lora_replay
