#pragma once

#include "host_sim/lora_params.hpp"
#include "host_sim/lora_replay/stage_processing.hpp"

#include <cstdint>
#include <vector>

namespace host_sim::lora_replay
{

HeaderDecodeResult try_decode_header(const std::vector<uint16_t>& symbols,
                                     std::size_t start,
                                     const host_sim::LoRaMetadata& meta);

bool probe_payload_crc(const std::vector<uint16_t>& symbols,
                       const HeaderDecodeResult& hdr,
                       const host_sim::LoRaMetadata& meta);

} // namespace host_sim::lora_replay
