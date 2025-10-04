#include "receiver.hpp"

#include <stdexcept>

namespace lora {

Receiver::Receiver(const DecodeParams &params)
    : params_(params),
      frame_sync_(params.sf, params.bandwidth_hz, params.sample_rate_hz),
      header_decoder_(params.sf, params.bandwidth_hz, params.sample_rate_hz),
      payload_decoder_(params.sf, params.bandwidth_hz, params.sample_rate_hz),
      sync_detector_(params.sf, params.bandwidth_hz, params.sample_rate_hz, params.sync_word) {
    if (params.sf < 5 || params.sf > 12) {
        throw std::invalid_argument("Spreading factor out of supported range (5-12)");
    }
}

DecodeResult Receiver::decode_samples(const std::vector<IqLoader::Sample> &samples) const {
    DecodeResult result;

    const auto sync = frame_sync_.synchronize(samples);
    result.frame_synced = sync.has_value();
    if (!result.frame_synced) {
        return result;
    }
    result.p_ofs_est = sync->p_ofs_est;

    const auto sync_word = sync_detector_.analyze(samples, sync->preamble_offset);
    if (!sync_word.has_value() || !sync_word->sync_ok) {
        return result;
    }

    std::optional<HeaderDecodeResult> header;
    if (params_.implicit_header) {
        if (params_.implicit_payload_length <= 0 || params_.implicit_cr < 1 || params_.implicit_cr > 4) {
            return result;
        }
        HeaderDecodeResult implicit{};
        implicit.fcs_ok = true;
        implicit.payload_length = params_.implicit_payload_length;
        implicit.has_crc = params_.implicit_has_crc;
        implicit.cr = params_.implicit_cr;
        implicit.implicit_header = true;
        header = implicit;
        result.header_ok = true;
    } else {
        header = header_decoder_.decode(samples, *sync);
        result.header_ok = header.has_value() && header->fcs_ok;
        if (!result.header_ok || !header.has_value()) {
            return result;
        }
    }
    result.header_payload_length = header->payload_length;

    const auto payload = payload_decoder_.decode(samples, *sync, *header, params_.ldro_enabled);
    if (!payload.has_value()) {
        return result;
    }

    result.payload_crc_ok = payload->crc_ok;
    result.payload = payload->bytes;
    result.raw_payload_symbols = payload->raw_symbols;
    result.success = result.payload_crc_ok;
    return result;
}

DecodeResult Receiver::decode_file(const std::filesystem::path &path) const {
    auto samples = IqLoader::load_cf32(path);
    return decode_samples(samples);
}

} // namespace lora
