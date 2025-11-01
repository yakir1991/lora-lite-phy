#include "receiver.hpp"

#include "sample_rate_resampler.hpp"

#include <algorithm>
#include <array>
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
    last_sample_rate_ratio_ = 1.0;
    result.sample_rate_ratio_used = 1.0;

    // 1) Preamble-based frame synchronization (timing and coarse CFO estimation).
    //    Returns preamble_offset, fine timing offset, and CFO estimate when found.
    const auto sync = frame_sync_.synchronize(samples);
    result.frame_synced = sync.has_value();
    if (!result.frame_synced) {
        // Early exit: nothing else to do if we didn't find a valid preamble.
        return result;
    }
    const double cfo_initial_hz = sync->cfo_hz;
    const double sample_rate_ratio_initial = (sync->sample_rate_ratio > 0.0) ? sync->sample_rate_ratio : 1.0;
    const double sample_rate_error_initial_ppm = sync->sample_rate_error_ppm;
    const double sample_rate_drift_initial = sync->sample_rate_drift_per_symbol;
    result.cfo_initial_hz = cfo_initial_hz;
    result.sample_rate_ratio_initial = sample_rate_ratio_initial;
    result.sample_rate_error_initial_ppm = sample_rate_error_initial_ppm;
    result.sample_rate_drift_per_symbol = sample_rate_drift_initial;
    FrameSyncResult sync_for_payload = *sync;
    const bool manual_sample_rate = params_.enable_sample_rate_correction;
    const double estimated_ratio = (sync_for_payload.sample_rate_ratio > 0.0) ? sync_for_payload.sample_rate_ratio : 1.0;
    if (manual_sample_rate) {
        sync_for_payload.sample_rate_ratio = params_.sample_rate_ratio;
    } else {
        sync_for_payload.sample_rate_ratio = estimated_ratio;
    }

    const double resample_threshold_ratio = std::max(0.0, params_.sample_rate_resample_threshold_ppm) * 1e-6;
    std::vector<IqLoader::Sample> corrected_samples;
    const std::vector<IqLoader::Sample> *active_samples = &samples;
    const std::optional<double> manual_ratio = manual_sample_rate ? std::optional<double>(params_.sample_rate_ratio) : std::nullopt;
    const double ratio_for_resample = manual_ratio.value_or(sync_for_payload.sample_rate_ratio > 0.0 ? sync_for_payload.sample_rate_ratio : 1.0);
    const bool resample_active = resample_threshold_ratio > 0.0 &&
                                 std::abs(ratio_for_resample - 1.0) > resample_threshold_ratio;
    if (resample_active) {
        corrected_samples = build_resampled_capture(samples, ratio_for_resample);
#ifndef NDEBUG
        std::fprintf(stderr, "[resample] ratio=%.9f p_ofs_before=%td size_in=%zu size_out=%zu\n",
                     ratio_for_resample, sync_for_payload.p_ofs_est,
                     samples.size(), corrected_samples.size());
#endif
        active_samples = &corrected_samples;
        auto scale_index = [&](std::ptrdiff_t value) -> std::ptrdiff_t {
            const double scaled = static_cast<double>(value) / ratio_for_resample;
            const auto rounded = static_cast<std::ptrdiff_t>(std::llround(scaled));
            return rounded;
        };
        sync_for_payload.preamble_offset = std::max<std::ptrdiff_t>(0, scale_index(sync_for_payload.preamble_offset));
        sync_for_payload.p_ofs_est = scale_index(sync_for_payload.p_ofs_est);
#ifndef NDEBUG
        std::fprintf(stderr, "[resample] scaled_p_ofs=%td\n", sync_for_payload.p_ofs_est);
#endif
        sync_for_payload.sample_rate_ratio = 1.0;
        sync_for_payload.sample_rate_error_ppm = 0.0;
        sync_for_payload.sample_rate_drift_per_symbol = 0.0;
    } else if (manual_ratio.has_value()) {
        sync_for_payload.sample_rate_ratio = *manual_ratio;
    }
    last_sample_rate_ratio_ = sync_for_payload.sample_rate_ratio;
    result.sample_rate_ratio_used = sync_for_payload.sample_rate_ratio;
    result.p_ofs_est = sync_for_payload.p_ofs_est;

    auto update_result_metrics = [&]() {
        result.cfo_final_hz = sync_for_payload.cfo_hz;
        result.sample_rate_error_ppm = sync_for_payload.sample_rate_error_ppm;
        result.sample_rate_drift_per_symbol = sync_for_payload.sample_rate_drift_per_symbol;
    };
    auto stash_payload_attempt = [&](const PayloadDecodeResult &attempt) {
        result.payload.assign(attempt.byte_view.begin(), attempt.byte_view.end());
        result.raw_payload_symbols.assign(attempt.raw_symbol_view.begin(), attempt.raw_symbol_view.end());
        result.payload_symbol_bins.assign(attempt.symbol_bin_view.begin(), attempt.symbol_bin_view.end());
        result.payload_degray_values.assign(attempt.degray_view.begin(), attempt.degray_view.end());
    };
    update_result_metrics();

    // 2) Optional sync word validation. Some capture flows may disable this
    //    to iterate faster or when sync word is unknown.
    if (!params_.skip_sync_word_check) {
        const auto sync_word = sync_detector_.analyze(samples, sync->preamble_offset, sync->cfo_hz);
        if (!sync_word.has_value() || !sync_word->sync_ok) {
            update_result_metrics();
            // Early exit if sync word doesn't match the configured one.
            return result;
        }
    }

    // 3) Header decode: either implicit (skip demod) or explicit (demod and CRC5).
    std::optional<HeaderDecodeResult> header;
    std::optional<PayloadDecodeResult> payload;
    if (params_.implicit_header) {
        // Implicit mode: the transmitter omits the header on-air. The receiver
        // must be configured with the payload length, code rate (CR), and whether
        // a CRC16 is present in the payload. We mark header FCS as OK by design.
        if (params_.implicit_payload_length <= 0 || params_.implicit_cr < 1 || params_.implicit_cr > 4) {
            update_result_metrics();
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
        header = header_decoder_.decode(*active_samples, sync_for_payload);
        result.header_ok = header.has_value() && header_result_sane(*header);
        update_result_metrics();
        if (!result.header_ok || !header.has_value()) {
            header = search_header(*active_samples, sync_for_payload);
            result.header_ok = header.has_value() && header_result_sane(*header);
            update_result_metrics();
        }
        if ((!result.header_ok || !header.has_value()) && params_.sf <= 6) {
            if (const auto brute = brute_force_header_and_payload(*active_samples, sync_for_payload)) {
                header = brute->header;
                payload = brute->payload;
                sync_for_payload = brute->sync;
                result.header_ok = true;
                update_result_metrics();
            }
        }
        if (!result.header_ok || !header.has_value()) {
            update_result_metrics();
            return result;
        }
    }
    result.header_payload_length = header->payload_length;
    result.p_ofs_est = sync_for_payload.p_ofs_est;

    const bool payload_ldro = params_.ldro_enabled_for_payload();

    // 4) Payload decode using timing/CFO from sync and parameters from header.
    //    Produces raw symbols (for debugging), dewhitened bytes, and CRC16 result.
    if (!payload.has_value()) {
        payload = payload_decoder_.decode(*active_samples, sync_for_payload, *header, payload_ldro, params_.soft_decoding);
#ifndef NDEBUG
        if (payload.has_value()) {
            std::fprintf(stderr, "[payload-try] crc_ok=%d bytes=%zu\n", payload->crc_ok ? 1 : 0, payload->bytes.size());
        } else {
            std::fprintf(stderr, "[payload-try] primary decode failed\n");
        }
#endif
    }
    if (!payload.has_value() || !payload->crc_ok) {
        auto recovered = search_payload(*active_samples, sync_for_payload, *header);
        update_result_metrics();
#ifndef NDEBUG
        if (recovered.has_value()) {
            std::fprintf(stderr, "[payload-search] crc_ok=%d bytes=%zu\n", recovered->crc_ok ? 1 : 0, recovered->bytes.size());
        } else {
            std::fprintf(stderr, "[payload-search] no result\n");
        }
#endif
        if (!recovered.has_value() || !recovered->crc_ok) {
            std::optional<PayloadDecodeResult> sr_recovered = search_payload_with_sample_rates(
                *active_samples, sync_for_payload, *header, manual_ratio);
            update_result_metrics();
#ifndef NDEBUG
            if (sr_recovered.has_value()) {
                std::fprintf(stderr, "[payload-sr] crc_ok=%d bytes=%zu\n", sr_recovered->crc_ok ? 1 : 0, sr_recovered->bytes.size());
            } else {
                std::fprintf(stderr, "[payload-sr] no result\n");
            }
#endif
            if (sr_recovered.has_value()) {
                payload = std::move(sr_recovered);
                update_result_metrics();
            } else {
                if (manual_ratio.has_value()) {
                    FrameSyncResult unity_sync = *sync;
                    unity_sync.sample_rate_ratio = 1.0;
                    unity_sync.sample_rate_error_ppm = 0.0;
                    unity_sync.sample_rate_drift_per_symbol = 0.0;
                    auto unity_payload = payload_decoder_.decode(samples, unity_sync, *header, payload_ldro, params_.soft_decoding);
#ifndef NDEBUG
                    if (unity_payload.has_value()) {
                        std::fprintf(stderr, "[payload-unity] crc_ok=%d bytes=%zu\n",
                                     unity_payload->crc_ok ? 1 : 0, unity_payload->bytes.size());
                    } else {
                        std::fprintf(stderr, "[payload-unity] no result\n");
                    }
#endif
                    if (!unity_payload.has_value() || !unity_payload->crc_ok) {
                        auto unity_recovered = search_payload(samples, unity_sync, *header);
#ifndef NDEBUG
                        if (unity_recovered.has_value()) {
                            std::fprintf(stderr, "[payload-unity-search] crc_ok=%d bytes=%zu\n",
                                         unity_recovered->crc_ok ? 1 : 0, unity_recovered->bytes.size());
                        } else {
                            std::fprintf(stderr, "[payload-unity-search] no result\n");
                        }
#endif
                        if (unity_recovered.has_value() && unity_recovered->crc_ok) {
                            unity_payload = std::move(unity_recovered);
                        }
                    }
                    if (unity_payload.has_value() && unity_payload->crc_ok) {
                        sync_for_payload = unity_sync;
                        update_result_metrics();
                        payload = std::move(unity_payload);
                        active_samples = &samples;
                        corrected_samples.clear();
                    } else {
                        if (recovered.has_value()) {
                            result.payload_crc_ok = recovered->crc_ok;
                            stash_payload_attempt(*recovered);
                        } else if (payload.has_value()) {
                            result.payload_crc_ok = payload->crc_ok;
                            stash_payload_attempt(*payload);
                        }
                        update_result_metrics();
                        return result;
                    }
                } else {
                    if (recovered.has_value()) {
                        result.payload_crc_ok = recovered->crc_ok;
                        stash_payload_attempt(*recovered);
                    } else if (payload.has_value()) {
                        result.payload_crc_ok = payload->crc_ok;
                        stash_payload_attempt(*payload);
                    }
                    update_result_metrics();
                    return result;
                }
            }
        } else {
            payload = std::move(recovered);
            update_result_metrics();
        }
    }

    result.payload_crc_ok = payload->crc_ok;
    result.payload = payload->bytes;
    result.raw_payload_symbols = payload->raw_symbols;
    result.payload_symbol_bins = payload->symbol_bins;
    result.payload_degray_values = payload->degray_values;
    result.sample_rate_ratio_used = sync_for_payload.sample_rate_ratio;
    result.cfo_final_hz = sync_for_payload.cfo_hz;
    result.sample_rate_error_ppm = sync_for_payload.sample_rate_error_ppm;
    result.sample_rate_drift_per_symbol = sync_for_payload.sample_rate_drift_per_symbol;
    result.ldro_used = payload_ldro;
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
    const bool payload_ldro = params_.ldro_enabled_for_payload();

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

        auto payload = payload_decoder_.decode(samples, trial_sync, header, payload_ldro, params_.soft_decoding);
        if (payload.has_value() && payload->crc_ok) {
            sync = trial_sync;
            return payload;
        }

        for (double delta = sweep_step; delta <= sweep_range; delta += sweep_step) {
            for (double signed_delta : {+delta, -delta}) {
                FrameSyncResult cfo_trial = trial_sync;
                cfo_trial.cfo_hz = sync.cfo_hz + signed_delta;
                payload = payload_decoder_.decode(samples, cfo_trial, header, payload_ldro, params_.soft_decoding);
                if (payload.has_value() && payload->crc_ok) {
                    sync = cfo_trial;
                    return payload;
                }
            }
        }
    }

    return std::nullopt;
}

std::vector<IqLoader::Sample> Receiver::build_resampled_capture(std::span<const IqLoader::Sample> samples,
                                                                double sample_rate_ratio) const {
    SampleRateResampler local_resampler;
    local_resampler.configure(sample_rate_ratio);
    if (!local_resampler.active()) {
        return std::vector<IqLoader::Sample>(samples.begin(), samples.end());
    }
    return local_resampler.resample(samples);
}

std::vector<double> Receiver::build_sample_rate_candidates(const FrameSyncResult &sync,
                                                           std::optional<double> extra_ratio) const {
    const double base = (sync.sample_rate_ratio > 0.0) ? sync.sample_rate_ratio : 1.0;
    static constexpr std::array<double, 7> ppm_offsets{0.0, -50.0, +50.0, -75.0, +75.0, -100.0, +100.0};

    std::vector<double> candidates;
    candidates.reserve(ppm_offsets.size() + 1);

    auto append_unique = [&](double ratio) {
        if (ratio <= 0.0) {
            return;
        }
        for (double existing : candidates) {
            if (std::abs(existing - ratio) < 1e-9) {
                return;
            }
        }
        candidates.push_back(ratio);
    };

    append_unique(base);
    append_unique(1.0);
    if (extra_ratio.has_value()) {
        append_unique(*extra_ratio);
    }
    for (double ppm : ppm_offsets) {
        const double ratio = base * (1.0 + ppm * 1e-6);
        append_unique(ratio);
    }

    return candidates;
}

std::optional<PayloadDecodeResult> Receiver::search_payload_with_sample_rates(const std::vector<IqLoader::Sample> &samples,
                                                                              FrameSyncResult &sync,
                                                                              const HeaderDecodeResult &header,
                                                                              std::optional<double> extra_ratio) const {
    const auto candidates = build_sample_rate_candidates(sync, extra_ratio);
    const bool payload_ldro = params_.ldro_enabled_for_payload();

    for (double ratio : candidates) {
        if (std::abs(ratio - sync.sample_rate_ratio) < 1e-9) {
            continue; // already attempted by caller
        }

        FrameSyncResult ratio_sync = sync;
        ratio_sync.sample_rate_ratio = ratio;
        ratio_sync.sample_rate_error_ppm = (ratio - 1.0) * 1e6;
        ratio_sync.sample_rate_drift_per_symbol =
            (ratio - 1.0) * static_cast<double>(frame_sync_.samples_per_symbol());

        HeaderDecodeResult header_trial = header;
        if (!header.implicit_header) {
            auto decoded_header = header_decoder_.decode(samples, ratio_sync);
            if (decoded_header.has_value() && header_result_sane(*decoded_header)) {
                header_trial = *decoded_header;
            }
        }

        #ifndef NDEBUG
        std::fprintf(stderr, "[sr-scan] ratio=%.9f\n", ratio);
        #endif
        auto payload = payload_decoder_.decode(samples, ratio_sync, header_trial, payload_ldro, params_.soft_decoding);
        if (payload.has_value() && payload->crc_ok) {
        #ifndef NDEBUG
            std::fprintf(stderr, "[sr-scan] ratio %.9f payload crc ok\n", ratio);
        #endif
            sync = ratio_sync;
            return payload;
        }

        auto recovered = search_payload(samples, ratio_sync, header_trial);
        if (recovered.has_value() && recovered->crc_ok) {
        #ifndef NDEBUG
            std::fprintf(stderr, "[sr-scan] ratio %.9f payload recovered via search\n", ratio);
        #endif
            sync = ratio_sync;
            return recovered;
        }
    }

    return std::nullopt;
}

std::optional<Receiver::BruteForceHeaderOutcome>
Receiver::brute_force_header_and_payload(const std::vector<IqLoader::Sample> &samples,
                                         const FrameSyncResult &sync) const {
    const int max_payload_len = 255;
    const std::array<bool, 2> crc_candidates{true, false};
    const std::size_t sps = frame_sync_.samples_per_symbol();
    const std::size_t payload_offset_samples = payload_decoder_.payload_symbol_offset_samples(false);
    const std::size_t sync_offset = (sync.p_ofs_est >= 0) ? static_cast<std::size_t>(sync.p_ofs_est) : 0;
    const std::size_t available_samples = samples.size() > sync_offset ? samples.size() - sync_offset : 0;
    if (available_samples <= payload_offset_samples) {
        return std::nullopt;
    }
    const std::size_t max_available_symbols = (available_samples - payload_offset_samples) / std::max<std::size_t>(1, sps);
    const bool payload_ldro = params_.ldro_enabled_for_payload();

    for (bool has_crc : crc_candidates) {
        for (int cr = 1; cr <= 4; ++cr) {
            for (int payload_len = 1; payload_len <= max_payload_len; ++payload_len) {
                HeaderDecodeResult guess{};
                guess.implicit_header = false;
                guess.fcs_ok = true;
                guess.payload_length = payload_len;
                guess.has_crc = has_crc;
                guess.cr = cr;

                const int required_symbols = payload_decoder_.compute_payload_symbol_count(guess, payload_ldro);
                if (required_symbols <= 0) {
                    continue;
                }
                if (static_cast<std::size_t>(required_symbols) > max_available_symbols) {
                    continue;
                }

                FrameSyncResult trial_sync = sync;
                auto attempt = payload_decoder_.decode(samples, trial_sync, guess, payload_ldro, params_.soft_decoding);
                if (!attempt.has_value()) {
                    continue;
                }
                if (guess.has_crc) {
                    if (!attempt->crc_ok) {
                        continue;
                    }
                } else {
                    if (static_cast<int>(attempt->bytes.size()) != payload_len) {
                        continue;
                    }
                }

                BruteForceHeaderOutcome outcome{
                    .header = guess,
                    .payload = *attempt,
                    .sync = trial_sync,
                };
                return outcome;
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
