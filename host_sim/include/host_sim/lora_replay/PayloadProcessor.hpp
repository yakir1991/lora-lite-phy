#pragma once

#include "host_sim/lora_params.hpp"
#include "host_sim/lora_replay/stage_processing.hpp"
#include <vector>
#include <cstdint>
#include <string>

namespace host_sim::lora_replay {

struct PayloadResult {
    std::vector<uint8_t> decoded_payload;
    std::vector<uint8_t> payload_bytes; // Raw bytes before whitening undo
    bool crc_ok = false;
    uint16_t decoded_crc = 0;
    uint16_t computed_crc = 0;
    StageOutputs stage_outputs;
};

class PayloadProcessor {
public:
    PayloadProcessor(const host_sim::LoRaMetadata& metadata);

    PayloadResult process(const std::vector<uint16_t>& symbols,
                          std::size_t symbol_cursor,
                          int payload_len,
                          int cr,
                          bool has_crc,
                          const HeaderDecodeResult& header,
                          const std::vector<uint16_t>& header_block);

private:
    host_sim::LoRaMetadata metadata_;
};

} // namespace host_sim::lora_replay
