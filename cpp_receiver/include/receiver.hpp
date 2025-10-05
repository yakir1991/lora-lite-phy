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
    int sf = 7;
    int bandwidth_hz = 125000;
    int sample_rate_hz = 500000;
    bool ldro_enabled = false;
    unsigned sync_word = 0x12;
    bool skip_sync_word_check = false;
    bool implicit_header = false;
    int implicit_payload_length = 0;
    bool implicit_has_crc = true;
    int implicit_cr = 1;
};

struct DecodeResult {
    bool success = false;
    bool frame_synced = false;
    bool header_ok = false;
    bool payload_crc_ok = false;
    std::vector<unsigned char> payload;
    std::vector<int> raw_payload_symbols;
    std::ptrdiff_t p_ofs_est = 0;
    int header_payload_length = 0;
};

class Receiver {
public:
    explicit Receiver(const DecodeParams &params);

    [[nodiscard]] DecodeResult decode_samples(const std::vector<IqLoader::Sample> &samples) const;
    [[nodiscard]] DecodeResult decode_file(const std::filesystem::path &path) const;

private:
    DecodeParams params_;
    FrameSynchronizer frame_sync_;
    HeaderDecoder header_decoder_;
    PayloadDecoder payload_decoder_;
    SyncWordDetector sync_detector_;
};

} // namespace lora
