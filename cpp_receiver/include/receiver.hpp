#pragma once

#include "frame_sync.hpp"
#include "header_decoder.hpp"
#include "iq_loader.hpp"
#include "sync_word_detector.hpp"
#include "payload_decoder.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace lora {

struct DecodeParams {
    // Spreading factor (5..12). Controls chips per symbol K=2^SF.
    int sf = 7;
    // Bandwidth in Hz (e.g., 125000).
    int bandwidth_hz = 125000;
    // Complex sample rate in Hz. Must be an integer multiple of bandwidth.
    int sample_rate_hz = 500000;
    // Low data rate optimization flag (DE/LDRO handling in payload demod).
    bool ldro_enabled = false;
    // LoRa 8-bit sync word (two 4-bit nibbles).
    unsigned sync_word = 0x12;
    // Skip validating the sync word (faster iterations / unknown sync).
    bool skip_sync_word_check = false;
    // Use implicit header mode: do not demodulate header, use provided fields below.
    bool implicit_header = false;
    // Required when implicit_header=true: payload length in bytes (>0).
    int implicit_payload_length = 0;
    // Required when implicit_header=true: whether payload includes CRC16.
    bool implicit_has_crc = true;
    // Required when implicit_header=true: code rate 1..4.
    int implicit_cr = 1;
};

struct DecodeResult {
    // True when full decode succeeded (payload CRC16 passed if present/expected).
    bool success = false;
    // True if preamble/frame sync succeeded.
    bool frame_synced = false;
    // True if header decode/validation succeeded (explicit mode only).
    bool header_ok = false;
    // True if payload CRC16 verified.
    bool payload_crc_ok = false;
    // Decoded payload bytes (message only).
    std::vector<unsigned char> payload;
    // Demodulated payload raw symbol bins (for debugging/analysis).
    std::vector<int> raw_payload_symbols;
    // Fine-aligned start index (samples) estimated by the synchronizer.
    std::ptrdiff_t p_ofs_est = 0;
    // Payload length parsed from the header (explicit) or implicit param.
    int header_payload_length = 0;
};

class Receiver {
public:
    // Construct the high-level LoRa receiver with decoding parameters.
    explicit Receiver(const DecodeParams &params);

    // Decode from an in-memory vector of complex samples.
    [[nodiscard]] DecodeResult decode_samples(const std::vector<IqLoader::Sample> &samples) const;
    // Convenience: load cf32 from disk and decode.
    [[nodiscard]] DecodeResult decode_file(const std::filesystem::path &path) const;

private:
    DecodeParams params_;
    FrameSynchronizer frame_sync_;
    HeaderDecoder header_decoder_;
    PayloadDecoder payload_decoder_;
    SyncWordDetector sync_detector_;
};

} // namespace lora
