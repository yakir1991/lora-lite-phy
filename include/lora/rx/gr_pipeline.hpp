#pragma once

#include <complex>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "lora/workspace.hpp"
#include "lora/rx/gr/header_decode.hpp"
#include "lora/rx/gr/utils.hpp"

namespace lora::rx::pipeline {

struct Config {
    uint32_t sf{7};
    size_t min_preamble_syms{8};
    float symbols_after_preamble{2.25f};
    size_t header_symbol_count{0};
    std::vector<int> os_candidates{4, 2, 1, 8};
    int sto_search{0};
    uint8_t expected_sync_word{0x34};
    bool decode_payload{true};
    bool expect_payload_crc{true};
    float bandwidth_hz{125000.0f};
    std::optional<bool> ldro_override{};
};

struct FrameSyncOutput {
    bool detected{false};
    size_t preamble_start_sample{0};
    int os{1};
    int phase{0};
    float cfo{0.0f};
    int sto{0};
    bool sync_detected{false};
    size_t sync_start_sample{0};
    size_t aligned_start_sample{0};
    size_t header_start_sample{0};
    std::vector<std::complex<float>> decimated;
    std::vector<std::complex<float>> compensated;
    std::vector<std::complex<float>> frame_samples;
};

struct FftDemodOutput {
    std::vector<uint32_t> raw_bins;
};

struct GrayMappingOutput {
    std::vector<uint32_t> reduced_symbols;
    std::vector<uint32_t> gray_symbols;
};

struct PayloadBitstream {
    std::vector<uint8_t> msb_first_bits;
    std::vector<uint8_t> deinterleaved_bits;
};

struct PayloadFecOutput {
    std::vector<uint8_t> nibbles;
    std::vector<uint8_t> raw_bytes;
};

struct HeaderStageOutput {
    std::vector<uint8_t> cw_bytes;
    std::vector<uint8_t> decoded_nibbles;
    std::vector<uint8_t> header_bytes;
    std::optional<lora::rx::gr::LocalHeader> header;
};

struct PayloadStageOutput {
    std::vector<uint8_t> dewhitened_payload;
    bool crc_ok{false};
};

struct PipelineResult {
    bool success{false};
    std::string failure_reason;
    FrameSyncOutput frame_sync;
    FftDemodOutput fft;
    HeaderStageOutput header;
    GrayMappingOutput gray;
    PayloadBitstream bits;
    PayloadFecOutput fec;
    PayloadStageOutput payload;
    
    // Multi-frame support
    size_t frame_count{0};
    std::vector<std::vector<uint8_t>> individual_frame_payloads;
    std::vector<bool> individual_frame_crc_ok;
};

class GnuRadioLikePipeline {
public:
    explicit GnuRadioLikePipeline(Config cfg = {});
    PipelineResult run(std::span<const std::complex<float>> samples);

private:
    Config cfg_;
    Workspace ws_;
    lora::rx::gr::Crc16Ccitt crc16_;
};

} // namespace lora::rx::pipeline
