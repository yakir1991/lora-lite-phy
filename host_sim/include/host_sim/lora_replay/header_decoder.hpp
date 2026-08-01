#pragma once

#include "host_sim/lora_params.hpp"
#include "host_sim/lora_replay/stage_processing.hpp"

#include <cstdint>
#include <vector>

namespace host_sim::lora_replay
{

/// CRC-16/CCITT-FALSE over the payload, as LoRa computes it (init 0x0000,
/// polynomial 0x1021, no final XOR).
uint16_t compute_raw_crc16(const std::vector<uint8_t>& payload);

/// Decode an explicit LoRa header starting at `start` in the symbol stream.
/// Check `.success` before using the result.
HeaderDecodeResult try_decode_header(const std::vector<uint16_t>& symbols, std::size_t start,
                                     const host_sim::LoRaMetadata& meta);

/// Outcome of decoding the payload that follows a header.
struct PayloadDecodeResult
{
    bool success{false};             ///< Enough symbols were present to recover `payload`.
    std::vector<uint8_t> payload;    ///< De-whitened payload, `payload_len` bytes.
    bool has_crc{false};             ///< Whether the frame carries a CRC.
    uint16_t crc_received{0};        ///< CRC read from the frame (only if has_crc).
    uint16_t crc_computed{0};        ///< CRC computed over the payload (only if has_crc).
    bool crc_ok{false};              ///< received == computed.
    std::size_t consumed_symbols{0}; ///< Symbol cursor after the payload.
};

/// Deinterleave, Hamming-decode and de-whiten the payload following `hdr`,
/// verifying the CRC when the frame carries one.
PayloadDecodeResult decode_payload(const std::vector<uint16_t>& symbols,
                                   const HeaderDecodeResult& hdr,
                                   const host_sim::LoRaMetadata& meta);

/// Decode the payload that follows `hdr` and report whether its CRC checks out.
/// Used to disambiguate candidate header positions.
bool probe_payload_crc(const std::vector<uint16_t>& symbols, const HeaderDecodeResult& hdr,
                       const host_sim::LoRaMetadata& meta);

} // namespace host_sim::lora_replay
