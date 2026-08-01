#pragma once

/// Packet-level LoRa decode.
///
/// The pieces below (burst detection, symbol alignment, FFT demodulation,
/// header and payload decode) have always been in the library, but callers had
/// to wire them together themselves — which meant reimplementing the frame
/// logic, usually without CRC verification. This header exposes the assembled
/// pipeline as one call.
///
/// It takes a sample buffer rather than a file, so it is usable from firmware
/// and from other hosts embedding the decoder.
///
/// Scope: burst detection, CFO-aware alignment and integer/fractional carrier
/// offset correction, explicit-header frames, CRC verification. It does *not*
/// yet do implicit-header mode (so SF6, which is always implicit, is out of
/// range), per-symbol SFO stride compensation, OS=2 upsample fallback, soft
/// decision, or multi-packet streaming. Over the golden over-the-air corpus it
/// recovers 30 of 39 captures; the ones it misses are the three SF6 implicit
/// frames and six high-SFO cases, and in every one of those it reports
/// crc_ok == false rather than returning a wrong payload. When you need the
/// full set of fallbacks, use the lora_replay tool, which layers them on top of
/// these same primitives.

#include "host_sim/lora_params.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace host_sim
{

/// A decoded LoRa frame, plus where it was found in the input.
struct DecodedPacket
{
    std::vector<uint8_t> payload; ///< De-whitened payload bytes.
    int payload_len{0};           ///< Length reported by the header.
    int cr{0};                    ///< Coding rate from the header (1..4 = 4/5..4/8).
    bool has_crc{false};          ///< Whether the frame carries a CRC.
    bool crc_ok{false};           ///< CRC verified (false when has_crc is false).
    uint16_t crc_received{0};
    uint16_t crc_computed{0};

    std::size_t burst_offset{0}; ///< Sample index where the burst was detected.
    std::size_t alignment{0};    ///< Sample index of the first aligned symbol.
    std::size_t consumed{0};     ///< Samples consumed through the end of the frame.
    float snr_db{0.0f};          ///< Rough estimate from burst power vs. leading noise.
};

/// Decode the first LoRa frame in `samples`.
///
/// `meta` supplies sf, bw, sample_rate and preamble_len; payload length, coding
/// rate and CRC presence come from the decoded header in explicit-header mode
/// and fall back to `meta` otherwise.
///
/// Returns std::nullopt when no burst is found, the buffer is too short, or the
/// header does not decode. A returned packet may still have crc_ok == false;
/// check it before trusting the payload.
std::optional<DecodedPacket> decode_packet(std::span<const std::complex<float>> samples,
                                           const LoRaMetadata& meta);

} // namespace host_sim
