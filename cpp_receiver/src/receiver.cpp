#include "receiver.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

std::vector<std::ptrdiff_t> build_header_offset_candidates(std::size_t sps) {
    std::vector<std::ptrdiff_t> offsets{0};
    const double fractions[] = {1.0 / 64.0, 1.0 / 32.0, 1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0, 1.0 / 2.0, 1.0, 2.0};
    for (double frac : fractions) {
        const auto step = static_cast<std::ptrdiff_t>(std::llround(frac * static_cast<double>(sps)));
        if (step <= 0) {
            continue;
        }
        offsets.push_back(step);
        offsets.push_back(-step);
    }
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    return offsets;
}

bool header_result_sane(const lora::HeaderDecodeResult &hdr) {
    if (!hdr.fcs_ok) {
        return false;
    }
    if (hdr.payload_length < 0 || hdr.payload_length > 255) {
        return false;
    }
    if (hdr.cr < 1 || hdr.cr > 4) {
        return false;
    }
    return true;
}

} // namespace

// This translation unit wires the core PHY building blocks (frame sync, header
// decoder, payload decoder, optional sync-word check) into a high-level
// `Receiver` facade. The goal is for embedding applications to have a single
// call that returns a rich `DecodeResult` while still being able to probe each
// stage during debugging via unit tests or CLI tooling.

namespace lora {

// Receiver orchestrates the complete LoRa PHY decoding pipeline.
// It wires together:
//  - FrameSynchronizer: detects the preamble and estimates timing/CFO
//  - SyncWordDetector: validates the LoRa sync word (optional)
//  - HeaderDecoder: demodulates and decodes the 8-symbol LoRa header
//  - PayloadDecoder: demodulates the payload and verifies CRC16
// The class exposes two entry points:
//  - decode_samples(): decode from an in-memory vector of complex samples
//  - decode_file():    load cf32 samples from disk and then decode
//
// Notes:
//  - Configuration is provided via DecodeParams (SF, BW, Fs, LDRO, sync word, etc.).
//  - Implicit header mode bypasses header demodulation and uses caller-provided
//    fields (payload length, CR, CRC presence). This matches LoRa's implicit header.
//  - On failure at any stage, DecodeResult indicates which checkpoint failed
//    (frame_synced, header_ok, payload_crc_ok) to aid debugging.
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

    // 1) Preamble-based frame synchronization (timing and coarse CFO estimation).
    //    Returns preamble_offset, fine timing offset, and CFO estimate when found.
    const auto sync = frame_sync_.synchronize(samples);
    result.frame_synced = sync.has_value();
    if (!result.frame_synced) {
        // Early exit: nothing else to do if we didn't find a valid preamble.
        return result;
    }
    FrameSyncResult sync_for_payload = *sync;
    result.p_ofs_est = sync_for_payload.p_ofs_est;

    // 2) Optional sync word validation. Some capture flows may disable this
    //    to iterate faster or when sync word is unknown.
    if (!params_.skip_sync_word_check) {
        const auto sync_word = sync_detector_.analyze(samples, sync->preamble_offset, sync->cfo_hz);
        if (!sync_word.has_value() || !sync_word->sync_ok) {
            // Early exit if sync word doesn't match the configured one.
            return result;
        }
    }

    // 3) Header decode: either implicit (skip demod) or explicit (demod and CRC5).
    std::optional<HeaderDecodeResult> header;
    if (params_.implicit_header) {
        // Implicit mode: the transmitter omits the header on-air. The receiver
        // must be configured with the payload length, code rate (CR), and whether
        // a CRC16 is present in the payload. We mark header FCS as OK by design.
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
        // Explicit mode: demodulate the 8-symbol header and verify its CRC5.
        header = header_decoder_.decode(samples, sync_for_payload);
        result.header_ok = header.has_value() && header_result_sane(*header);
        if (!result.header_ok || !header.has_value()) {
            header = search_header(samples, sync_for_payload);
            result.header_ok = header.has_value() && header_result_sane(*header);
            if (!result.header_ok || !header.has_value()) {
                return result;
            }
        }
    }
    result.header_payload_length = header->payload_length;
    result.p_ofs_est = sync_for_payload.p_ofs_est;

    // 4) Payload decode using timing/CFO from sync and parameters from header.
    //    Produces raw symbols (for debugging), dewhitened bytes, and CRC16 result.
    auto payload = payload_decoder_.decode(samples, sync_for_payload, *header, params_.ldro_enabled);
    if (!payload.has_value() || !payload->crc_ok) {
        auto recovered = search_payload(samples, sync_for_payload, *header);
        if (recovered.has_value()) {
            payload = std::move(recovered);
        } else {
            if (payload.has_value()) {
                result.payload_crc_ok = payload->crc_ok;
            }
            return result;
        }
    }

    result.payload_crc_ok = payload->crc_ok;
    result.payload = payload->bytes;
    result.raw_payload_symbols = payload->raw_symbols;
    // Success requires payload CRC16 to pass; other flags indicate partial progress.
    result.success = result.payload_crc_ok;
    return result;
}

std::optional<HeaderDecodeResult> Receiver::search_header(const std::vector<IqLoader::Sample> &samples,
                                                          FrameSyncResult &sync) const {
    const std::size_t sps = frame_sync_.samples_per_symbol();
    const auto offsets = build_header_offset_candidates(sps);

    const double base_cfo = sync.cfo_hz;
    double sweep_range = std::max(params_.header_cfo_range_hz, std::max(200.0, std::abs(base_cfo) * 2.5));
    double sweep_step = params_.header_cfo_step_hz > 0.0
                            ? std::min(params_.header_cfo_step_hz, sweep_range / 20.0)
                            : sweep_range / 40.0;
    if (sweep_step <= 0.0) {
        sweep_step = sweep_range / 40.0;
    }
    sweep_step = std::max(2.0, sweep_step);

    for (std::ptrdiff_t offset : offsets) {
        FrameSyncResult trial_sync = sync;
        trial_sync.p_ofs_est += offset;

        auto header = header_decoder_.decode(samples, trial_sync);
        if (header.has_value() && header_result_sane(*header)) {
            sync = trial_sync;
            return header;
        }

        // CFO sweep around the synchronizer estimate.
        for (double delta = sweep_step; delta <= sweep_range; delta += sweep_step) {
            for (double signed_delta : {+delta, -delta}) {
                FrameSyncResult cfo_trial = trial_sync;
                cfo_trial.cfo_hz = sync.cfo_hz + signed_delta;
                header = header_decoder_.decode(samples, cfo_trial);
                if (header.has_value() && header_result_sane(*header)) {
                    sync = cfo_trial;
                    return header;
                }
            }
        }
    }

    return std::nullopt;
}

std::optional<PayloadDecodeResult> Receiver::search_payload(const std::vector<IqLoader::Sample> &samples,
                                                          FrameSyncResult &sync,
                                                          const HeaderDecodeResult &header) const {
    const std::size_t sps = frame_sync_.samples_per_symbol();
    const auto offsets = build_header_offset_candidates(sps);

    const double base_cfo = sync.cfo_hz;
    double sweep_range = std::max(params_.header_cfo_range_hz, std::max(200.0, std::abs(base_cfo) * 2.5));
    double sweep_step = params_.header_cfo_step_hz > 0.0
                            ? std::min(params_.header_cfo_step_hz, sweep_range / 20.0)
                            : sweep_range / 40.0;
    if (sweep_step <= 0.0) {
        sweep_step = sweep_range / 40.0;
    }
    sweep_step = std::max(2.0, sweep_step);

    for (std::ptrdiff_t offset : offsets) {
        FrameSyncResult trial_sync = sync;
        trial_sync.p_ofs_est += offset;

        auto payload = payload_decoder_.decode(samples, trial_sync, header, params_.ldro_enabled);
        if (payload.has_value() && payload->crc_ok) {
            sync = trial_sync;
            return payload;
        }

        for (double delta = sweep_step; delta <= sweep_range; delta += sweep_step) {
            for (double signed_delta : {+delta, -delta}) {
                FrameSyncResult cfo_trial = trial_sync;
                cfo_trial.cfo_hz = sync.cfo_hz + signed_delta;
                payload = payload_decoder_.decode(samples, cfo_trial, header, params_.ldro_enabled);
                if (payload.has_value() && payload->crc_ok) {
                    sync = cfo_trial;
                    return payload;
                }
            }
        }
    }

    return std::nullopt;
}

DecodeResult Receiver::decode_file(const std::filesystem::path &path) const {
    // Convenience wrapper: load interleaved cf32 samples from disk and decode.
    auto samples = IqLoader::load_cf32(path);
    return decode_samples(samples);
}

} // namespace lora
