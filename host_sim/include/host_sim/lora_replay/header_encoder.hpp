#pragma once

#include "host_sim/lora_params.hpp"

#include <cstdint>
#include <vector>

namespace host_sim::lora_replay
{

uint8_t compute_header_checksum(int payload_len, bool has_crc, int cr);

std::vector<uint8_t> build_header_nibbles(int payload_len, bool has_crc, int cr);

std::vector<uint16_t> encode_header_symbols(int sf,
                                            bool ldro,
                                            int payload_len,
                                            bool has_crc,
                                            int cr);

std::vector<uint16_t> encode_header_symbols(const host_sim::LoRaMetadata& meta,
                                            int payload_len,
                                            bool has_crc,
                                            int cr);

std::vector<uint8_t> build_payload_with_crc(const std::vector<uint8_t>& payload);

} // namespace host_sim::lora_replay
