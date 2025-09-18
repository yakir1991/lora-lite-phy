#pragma once

#include <complex>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "lora/rx/header.hpp"
#include "lora/workspace.hpp"
#include "lora/utils/hamming.hpp"

namespace lora::rx::pipeline {

struct Config {
    uint32_t sf{7};
    size_t min_preamble_syms{8};
    float symbols_after_preamble{2.25f};
    size_t header_symbol_count{16};
    std::vector<int> os_candidates{1, 2, 4, 8};
    int sto_search{0};
    bool decode_payload{true};
    bool expect_payload_crc{true};
};

struct FrameSyncOutput {
    bool detected{false};
    size_t preamble_start_sample{0};
    int os{1};
    int phase{0};
    float cfo{0.0f};
    int sto{0};
    size_t header_start_sample{0};
    std::vector<std::complex<float>> decimated;
    std::vector<std::complex<float>> compensated;
    std::vector<std::complex<float>> frame_samples;
};

struct FftDemodOutput {
    std::vector<uint32_t> raw_bins;
};

struct GrayMappingOutput {
    std::vector<uint32_t> payload_symbols;
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
    std::optional<lora::rx::LocalHeader> header;
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
};

class GnuRadioLikePipeline {
public:
    explicit GnuRadioLikePipeline(Config cfg = {});
    PipelineResult run(std::span<const std::complex<float>> samples);

private:
    Config cfg_;
    Workspace ws_;
    lora::utils::HammingTables hamming_tables_;
};

} // namespace lora::rx::pipeline

