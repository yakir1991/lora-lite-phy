#pragma once

#include "host_sim/lora_params.hpp"
#include "host_sim/lora_replay/stage_processing.hpp" // For HeaderDecodeResult
#include <vector>
#include <cstdint>
#include <optional>

namespace host_sim::lora_replay {

struct HeaderSearchResult {
    HeaderDecodeResult header;
    std::size_t chosen_offset = 0;
    std::size_t symbol_cursor = 0;
    bool success = false;
    bool checksum_valid = false;
};

class HeaderSearcher {
public:
    HeaderSearcher(const host_sim::LoRaMetadata& metadata, bool debug_headers = false);

    HeaderSearchResult search(const std::vector<uint16_t>& symbols,
                              std::optional<std::size_t> forced_header_start,
                              bool external_sync_active);

private:
    host_sim::LoRaMetadata metadata_;
    bool debug_headers_;

    bool attempt_header(const std::vector<uint16_t>& symbols,
                        std::size_t candidate,
                        std::optional<std::size_t> forced_header_start,
                        HeaderSearchResult& result,
                        HeaderDecodeResult& best_header,
                        std::size_t& best_header_offset,
                        int& best_header_score);
};

} // namespace host_sim::lora_replay
