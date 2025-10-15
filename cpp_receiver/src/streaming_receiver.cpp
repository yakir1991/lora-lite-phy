#include "streaming_receiver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

// The streaming receiver keeps enough rolling state to decode arbitrarily long
// IQ streams while emitting structured events (sync/header/payload bytes).
// Compared to the batch `Receiver`, it needs extra bookkeeping: sliding buffers,
// guarded consumption, and per-frame metadata. The comments here aim to walk
// through each decision point so maintainers can reason about buffer sizes and
// latency trade-offs. Each helper intentionally answers a “why is this safe?”
// question so refactors do not accidentally break synchronizer invariants.

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

namespace lora {

StreamingReceiver::StreamingReceiver(const DecodeParams &params)
    : params_(params),
      synchronizer_(params.sf, params.bandwidth_hz, params.sample_rate_hz),
      header_decoder_(params.sf, params.bandwidth_hz, params.sample_rate_hz),
      payload_decoder_(params.sf, params.bandwidth_hz, params.sample_rate_hz) {
    // Parameter sanity checks mirror those in the synchronizer/decoders to fail fast
    if (params.sf < 5 || params.sf > 12) {
        throw std::invalid_argument("Spreading factor out of supported range (5-12)");
    }
    if (params.bandwidth_hz <= 0 || params.sample_rate_hz <= 0) {
        throw std::invalid_argument("Bandwidth and sample rate must be positive");
    }
    if (params.sample_rate_hz % params.bandwidth_hz != 0) {
        throw std::invalid_argument("Sample rate must be an integer multiple of bandwidth");
    }

    // Precompute samples per symbol (sps) for offset calculations. We assume
    // integer oversampling so sps = (2^SF) * (Fs/BW)
    const std::size_t chips_per_symbol = static_cast<std::size_t>(1) << params.sf;
    sps_ = chips_per_symbol * static_cast<std::size_t>(params.sample_rate_hz) / static_cast<std::size_t>(params.bandwidth_hz);
}

std::vector<StreamingReceiver::FrameEvent>
StreamingReceiver::push_samples(std::span<const Sample> chunk) {
    // High-level flow:
    //  1) Feed new samples to the streaming synchronizer (which keeps its own buffer).
    //  2) Mirror the growth of the synchronizer buffer into our local `capture_` so that
    //     indices line up; keep `capture_` size bounded to synchronizer's buffer.
    //  3) If no frame is pending, try to start one from a new detection and emit Sync event.
    //  4) When enough samples are available, decode header; emit HeaderDecoded.
    //  5) When enough for payload, decode; optionally stream bytes; then emit terminal event
    //     (FrameDone or FrameError), advance buffers, and clear pending.
    std::vector<FrameEvent> events;
    if (chunk.empty()) {
        return events;
    }

    // Observe synchronizer buffer state before/after update to derive the appended tail.
    const auto &buffer_before = synchronizer_.buffer();
    const std::size_t buffer_before_size = buffer_before.size();
    // Forward scratch-aware detection: reuses embedded buffers when available.
    const auto detection = synchronizer_.update(chunk);
    const auto &buffer = synchronizer_.buffer();
    const std::size_t buffer_after_size = buffer.size();
    const std::size_t global_offset = synchronizer_.buffer_global_offset();

    const bool emit_payload_bytes = params_.emit_payload_bytes;

    // Keep `capture_` aligned with the synchronizer buffer by appending the same
    // newly-added tail (when possible), otherwise fall back to appending the raw chunk.
    if (buffer_after_size > buffer_before_size) {
        const std::size_t appended = buffer_after_size - buffer_before_size;
        capture_.insert(capture_.end(), buffer.end() - static_cast<std::ptrdiff_t>(appended), buffer.end());
    } else {
        capture_.insert(capture_.end(), chunk.begin(), chunk.end());
    }

    // Bound `capture_` to not exceed the synchronizer's buffer only when no frame is pending.
    // While a frame is active we retain all samples to ensure the payload window can be reached
    // even if the synchronizer buffer trims its own prefix.
    if (!pending_) {
        if (capture_.size() > buffer_after_size) {
            const std::size_t drop = capture_.size() - buffer_after_size;
            capture_.erase(capture_.begin(), capture_.begin() + static_cast<std::ptrdiff_t>(drop));
            capture_global_offset_ += drop;
        }
    }

    // No active frame: start one when a detection is available and emit SyncAcquired.
    if (!pending_) {
        if (detection.has_value()) {
            pending_.emplace();
            pending_->sync = *detection;
            const std::size_t buffer_base = capture_.size() >= buffer_after_size ? capture_.size() - buffer_after_size : 0;
            pending_->preamble_offset = buffer_base + static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, detection->preamble_offset));
            pending_->global_sample_index = capture_global_offset_ + pending_->preamble_offset;

#ifndef NDEBUG
            std::fprintf(stderr,
                         "[stream] sync acquired: preamble_offset=%zu global=%zu buffer_size=%zu capture_size=%zu p_ofs=%td\n",
                         pending_->preamble_offset,
                         pending_->global_sample_index,
                         buffer_after_size,
                         capture_.size(),
                         pending_->sync.p_ofs_est);
#endif

            FrameEvent ev;
            ev.type = FrameEvent::Type::SyncAcquired;
            ev.global_sample_index = pending_->global_sample_index;
            ev.sync = pending_->sync;
            events.push_back(ev);
            pending_->sync_reported = true;
        } else {
            // Nothing to do yet; keep accumulating samples.
            return events;
        }
    }

    if (!pending_) {
        return events;
    }

    auto &frame = *pending_;

    // Header stage:
    //  - In explicit mode, wait until enough samples exist and attempt header decode.
    //  - In implicit (embedded) mode, synthesize header metadata from params immediately.
    if (!frame.header) {
        if (params_.implicit_header) {
            // Synthesize a minimal header using implicit params; mark as available
            HeaderDecodeResult hdr;
            hdr.implicit_header = true;
            hdr.payload_length = std::max(0, params_.implicit_payload_length);
            hdr.has_crc = params_.implicit_has_crc;
            hdr.cr = std::clamp(params_.implicit_cr, 1, 4);
            frame.header = hdr;
            // Use local sync aligned to preamble start
            frame.sync = make_local_sync(frame.sync, frame.preamble_offset);
            // Compute required samples from preamble to end of payload window
            const bool ldro_enabled = params_.ldro_enabled;
            const int payload_syms = compute_payload_symbol_count(*frame.header, ldro_enabled);
            if (payload_syms > 0) {
                const std::size_t guard = frame.sync.p_ofs_est >= 0 ? static_cast<std::size_t>(frame.sync.p_ofs_est) : 0;
                frame.samples_needed = guard + payload_offset_samples() + static_cast<std::size_t>(payload_syms) * sps_;
#ifndef NDEBUG
                std::fprintf(stderr,
                             "[stream] (implicit) payload symbols=%d samples_needed=%zu guard=%zu capture_size=%zu\n",
                             payload_syms,
                             frame.samples_needed,
                             guard,
                             capture_.size());
#endif
            } else {
                FrameEvent err;
                err.type = FrameEvent::Type::FrameError;
                err.global_sample_index = frame.global_sample_index;
                err.message = "invalid payload symbol count";
                events.push_back(err);
                finalize_frame(frame.preamble_offset + frame.samples_needed);
                pending_.reset();
                return events;
            }
        } else if (header_ready(frame, capture_)) {
            // Try multiple small offsets around the estimated p_ofs to improve robustness
            // against rounding/jitter. We keep attempts bounded and stop at first success.
            HeaderDecoder::MutableIntSpan header_symbol_span;
            HeaderDecoder::MutableIntSpan header_bits_span;
    #ifdef LORA_EMBEDDED_PROFILE
            static std::vector<int> header_symbol_storage;
            static std::vector<int> header_bits_storage;
            header_symbol_storage.resize(8);
            header_bits_storage.resize(32);
            header_symbol_span = {header_symbol_storage.data(), header_symbol_storage.size()};
            header_bits_span = {header_bits_storage.data(), header_bits_storage.size()};
    #endif
            const std::vector<Sample> view(capture_.begin() + frame.preamble_offset, capture_.end());

            // Optional: dump an IQ slice covering preamble through end-of-header and extra payload for diagnostics
            auto dump_header_iq = [&](const FrameSyncResult &sync_local,
                                     const HeaderDecodeResult *hdr_opt,
                                     std::ptrdiff_t cand_ofs_samples,
                                     int attempts_tried) {
                if (params_.dump_header_iq_path.empty()) return;
                // Compute slice [abs_start, abs_end) relative to capture_ base to include more preamble.
                const std::size_t preamble_extra_syms = 16u; // could be parameterized
                const std::size_t extra_guard = sps_ / 2; // include some post guard
                const std::size_t pre_guard = std::min<std::size_t>(preamble_extra_syms * sps_, frame.preamble_offset);
                const std::size_t abs_start = frame.preamble_offset - pre_guard;
                // End after header + configurable payload symbols
                const std::size_t payload_syms_after = params_.dump_header_iq_payload_syms > 0 ? static_cast<std::size_t>(params_.dump_header_iq_payload_syms) : 64u;
                const std::size_t header_base = frame.preamble_offset + (sync_local.p_ofs_est >= 0 ? static_cast<std::size_t>(sync_local.p_ofs_est) : 0) + header_offset_samples();
                const std::size_t abs_end_target = header_base + 8u * sps_ + payload_syms_after * sps_ + extra_guard;
                const std::size_t abs_end = std::min(abs_end_target, capture_.size());
                if (abs_end <= abs_start || abs_end > capture_.size()) return;
                // Write as raw cf32 (interleaved float32 I, Q)
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(params_.dump_header_iq_path).parent_path(), ec);
                std::ofstream ofs(params_.dump_header_iq_path, std::ios::binary);
                if (!ofs) return;
                for (std::size_t i = abs_start; i < abs_end; ++i) {
                    const float re = capture_[i].real();
                    const float im = capture_[i].imag();
                    ofs.write(reinterpret_cast<const char*>(&re), sizeof(float));
                    ofs.write(reinterpret_cast<const char*>(&im), sizeof(float));
                }
                // Meta sidecar as JSON next to the cf32 file
                try {
                    const std::filesystem::path meta_path = std::filesystem::path(params_.dump_header_iq_path).concat(".meta.json");
                    std::ofstream meta(meta_path);
                    if (meta) {
                        meta << "{\n";
                        meta << "  \"sf\": " << params_.sf << ",\n";
                        meta << "  \"bw\": " << params_.bandwidth_hz << ",\n";
                        meta << "  \"fs\": " << params_.sample_rate_hz << ",\n";
                        meta << "  \"cr\": " << (hdr_opt ? hdr_opt->cr : params_.implicit_cr) << ",\n";
                        meta << "  \"has_crc\": " << (hdr_opt ? (hdr_opt->has_crc ? 1 : 0) : (params_.implicit_has_crc ? 1 : 0)) << ",\n";
                        meta << "  \"impl_header\": " << (params_.implicit_header ? 1 : 0) << ",\n";
                        meta << "  \"ldro_mode\": " << (params_.ldro_enabled ? 1 : 0) << ",\n";
                        meta << "  \"sync_word\": " << static_cast<unsigned>(params_.sync_word) << ",\n";
                        meta << "  \"payload_len\": " << (hdr_opt ? hdr_opt->payload_length : params_.implicit_payload_length) << ",\n";
                        meta << "  \"cfo_used_hz\": " << sync_local.cfo_hz << ",\n";
                        meta << "  \"p_ofs_est\": " << sync_local.p_ofs_est << ",\n";
                        meta << "  \"slice_start\": " << static_cast<long long>(abs_start) << ",\n";
                        meta << "  \"slice_end\": " << static_cast<long long>(abs_end) << ",\n";
                        meta << "  \"cand_offset_samples\": " << static_cast<long long>(cand_ofs_samples) << ",\n";
                        meta << "  \"attempts\": " << attempts_tried << ",\n";
                        meta << "  \"header_bins\": [";
                        if (hdr_opt) {
                            for (std::size_t i = 0; i < hdr_opt->raw_symbol_view.size(); ++i) {
                                meta << hdr_opt->raw_symbol_view[i];
                                if (i + 1 < hdr_opt->raw_symbol_view.size()) meta << ",";
                            }
                        }
                        meta << "],\n";
                        meta << "  \"header_ok\": " << (hdr_opt ? (hdr_opt->fcs_ok ? 1 : 0) : 0) << "\n";
                        meta << "}\n";
                    }
                } catch (...) {
                    // Best-effort; ignore meta write failures
                }
            };

            const auto candidates = build_header_offset_candidates(sps_);
            FrameSyncResult best_sync = make_local_sync(frame.sync, frame.preamble_offset);
            const double sweep_range = std::max(params_.header_cfo_range_hz,
                                                std::max(200.0, std::abs(best_sync.cfo_hz) * 2.5));
            double sweep_step = params_.header_cfo_step_hz > 0.0
                                    ? std::min(params_.header_cfo_step_hz, sweep_range / 20.0)
                                    : sweep_range / 40.0;
            if (sweep_step <= 0.0) {
                sweep_step = sweep_range / 40.0;
            }
            sweep_step = std::max(2.0, sweep_step);


            std::optional<HeaderDecodeResult> header;

            int attempts = 0;
            bool header_found = false;

            auto attempt_decode = [&](const FrameSyncResult &candidate_sync) -> bool {
                auto hdr = header_decoder_.decode(view, candidate_sync, header_symbol_span, header_bits_span);
                if (hdr.has_value() && header_result_sane(*hdr)) {
                    frame.sync = candidate_sync;
                    header = std::move(hdr);
                    return true;
                }
                return false;
            };

            for (std::ptrdiff_t cand : candidates) {
                FrameSyncResult trial_sync = best_sync;
                trial_sync.p_ofs_est += cand;
                ++attempts;
#ifndef NDEBUG
                std::fprintf(stderr,
                             "[stream] attempting header decode: preamble_offset=%zu capture_size=%zu p_ofs_est_local=%td (cand=%td)\n",
                             frame.preamble_offset,
                             capture_.size(),
                             trial_sync.p_ofs_est,
                             cand);
#endif
                if (attempt_decode(trial_sync)) {
                    dump_header_iq(frame.sync, &*header, cand, attempts);
                    header_found = true;
                    break;
                }

                for (double delta = sweep_step; delta <= sweep_range && !header_found; delta += sweep_step) {
                    for (double signed_delta : {+delta, -delta}) {
                        FrameSyncResult cfo_trial = trial_sync;
                        cfo_trial.cfo_hz += signed_delta;
                        if (attempt_decode(cfo_trial)) {
                            dump_header_iq(frame.sync, &*header, cand, attempts);
                            header_found = true;
                            break;
                        }
                    }
                }

                if (!header_found && params_.dump_header_iq_always) {
                    dump_header_iq(trial_sync, nullptr, cand, attempts);
                }

                if (header_found) {
                    break;
                }
            }

            if (!header_found) {
#ifndef NDEBUG
                std::fprintf(stderr,
                             "[stream] header decode failed at preamble=%zu capture_size=%zu (tried %d candidates)\n",
                             frame.preamble_offset,
                             capture_.size(),
                             attempts);
#endif
            } else {
                frame.header = header;
            }

#ifndef NDEBUG
                std::fprintf(stderr,
                             "[stream] header decoded: payload_len=%d\n",
                             frame.header->payload_length);
#endif

                // Predict the number of payload symbols and compute total samples needed
                // from the preamble start to cover the entire payload window.
                const bool ldro_enabled = params_.ldro_enabled;
                const int payload_syms = compute_payload_symbol_count(*header, ldro_enabled);
                if (payload_syms > 0) {
                    const std::size_t guard = frame.sync.p_ofs_est >= 0 ? static_cast<std::size_t>(frame.sync.p_ofs_est) : 0;
                    frame.samples_needed = guard + payload_offset_samples() + static_cast<std::size_t>(payload_syms) * sps_;
#ifndef NDEBUG
                    std::fprintf(stderr,
                                 "[stream] payload symbols=%d samples_needed=%zu guard=%zu capture_size=%zu\n",
                                 payload_syms,
                                 frame.samples_needed,
                                 guard,
                                 capture_.size());
#endif
                } else {
                    // Defensive: impossible/invalid header (e.g., zero or negative symbols)
                    FrameEvent err;
                    err.type = FrameEvent::Type::FrameError;
                    err.global_sample_index = frame.global_sample_index;
                    err.message = "invalid payload symbol count";
                    events.push_back(err);
                    finalize_frame(frame.preamble_offset + frame.samples_needed);
                    pending_.reset();
                    return events;
                }
        }
    }

    // Emit header event once (as soon as it's available)
    if (frame.header && !frame.header_reported) {
        FrameEvent ev;
        ev.type = FrameEvent::Type::HeaderDecoded;
        ev.global_sample_index = frame.global_sample_index + static_cast<std::size_t>(frame.sync.p_ofs_est);
        ev.sync = frame.sync;
        ev.header = frame.header;
        events.push_back(ev);
        frame.header_reported = true;
    }

    // Payload stage: once all samples are present for the payload window, decode it.
    if (frame.header && payload_ready(frame, capture_)) {
#ifndef NDEBUG
        std::fprintf(stderr,
                     "[stream] attempting payload decode: samples_needed=%zu capture_size=%zu\n",
                     frame.samples_needed,
                     capture_.size());
#endif
        const std::vector<Sample> view(capture_.begin() + frame.preamble_offset,
                                       capture_.begin() + frame.preamble_offset + frame.samples_needed);
    PayloadDecoder::MutableByteSpan payload_span;
    PayloadDecoder::MutableIntSpan raw_symbol_span;
#ifdef LORA_EMBEDDED_PROFILE
    static std::vector<unsigned char> payload_storage;
    static std::vector<int> raw_symbol_storage;
    payload_storage.resize(static_cast<std::size_t>(frame.header->payload_length));
    const int symbol_count = payload_decoder_.compute_payload_symbol_count(*frame.header, params_.ldro_enabled);
    raw_symbol_storage.resize(symbol_count > 0 ? static_cast<std::size_t>(symbol_count) : 0);
    if (!payload_storage.empty()) {
        payload_span = {payload_storage.data(), payload_storage.size()};
    }
    if (!raw_symbol_storage.empty()) {
        raw_symbol_span = {raw_symbol_storage.data(), raw_symbol_storage.size()};
    }
#endif
    const auto payload = payload_decoder_.decode(view, frame.sync, *frame.header, params_.ldro_enabled, payload_span, raw_symbol_span);

        FrameEvent ev;
        ev.global_sample_index = frame.global_sample_index + frame.samples_needed;
        if (payload.has_value()) {
            // Optionally stream per-byte events for clients that want progressive output.
            if (emit_payload_bytes) {
        for (unsigned char b : payload->byte_view) {
                    FrameEvent byte_ev;
                    byte_ev.type = FrameEvent::Type::PayloadByte;
                    byte_ev.global_sample_index = frame.global_sample_index + frame.samples_needed;
                    byte_ev.payload_byte = b;
                    events.push_back(byte_ev);
                }
            }

            // Populate final result with metadata needed by higher layers.
            DecodeResult result;
            result.success = payload->crc_ok;
            result.frame_synced = true;
            result.header_ok = true;
            result.payload_crc_ok = payload->crc_ok;
            result.payload.assign(payload->byte_view.begin(), payload->byte_view.end());
            result.raw_payload_symbols.assign(payload->raw_symbol_view.begin(), payload->raw_symbol_view.end());
            result.p_ofs_est = frame.sync.p_ofs_est;
            result.header_payload_length = frame.header->payload_length;

            ev.type = FrameEvent::Type::FrameDone;
            ev.result = std::move(result);
#ifndef NDEBUG
            std::fprintf(stderr, "[stream] payload decode success (bytes=%zu)\n", ev.result->payload.size());
#endif
        } else {
            // Terminal failure for this frame
            ev.type = FrameEvent::Type::FrameError;
            ev.message = "payload decode failed";
#ifndef NDEBUG
            std::fprintf(stderr, "[stream] payload decode failed\n");
#endif
        }
        events.push_back(ev);

        // Advance all buffers past this frame and clear the pending state.
        finalize_frame(frame.preamble_offset + frame.samples_needed);
        pending_.reset();
    }

    return events;
}

void StreamingReceiver::reset() {
    // Reset both the synchronizer state and our mirrored capture buffer.
    synchronizer_.reset();
    capture_.clear();
    capture_global_offset_ = 0;
    pending_.reset();
}

bool StreamingReceiver::header_ready(const PendingFrame &frame, const std::vector<Sample> &buffer) const {
    // Header can be attempted once we have:
    //   preamble_offset (from capture start) + guard (fractional p_ofs_est if positive)
    // + header_offset (preamble-to-header) + header symbol span
    const std::ptrdiff_t guard = frame.sync.p_ofs_est;
    const std::size_t guard_pos = guard >= 0 ? static_cast<std::size_t>(guard) : 0;
    const std::size_t need = frame.preamble_offset + guard_pos + header_offset_samples() + header_decoder_.symbol_span_samples();
    return buffer.size() >= need;
}

bool StreamingReceiver::payload_ready(const PendingFrame &frame, const std::vector<Sample> &buffer) const {
    // Payload can be attempted once the full computed span from preamble to
    // the end of payload has arrived in the capture buffer.
    const std::size_t need = frame.preamble_offset + frame.samples_needed;
    return buffer.size() >= need;
}

std::size_t StreamingReceiver::header_offset_samples() const {
    // Distance (in samples) from preamble start to header start:
    //   Nrise (frontend/windowing rise) + 12 symbols of preamble + 1/4 symbol guard
    const double fs = static_cast<double>(params_.sample_rate_hz);
    const std::size_t Nrise = static_cast<std::size_t>(std::ceil(50e-6 * fs));
    return Nrise + (12u * sps_) + (sps_ / 4u);
}

std::size_t StreamingReceiver::payload_offset_samples() const {
    // In explicit mode, payload starts after 8 header symbols.
    // In implicit (embedded) mode, payload follows immediately after preamble.
    const std::size_t header_syms = params_.implicit_header ? 0u : 8u;
    return header_offset_samples() + header_syms * sps_;
}

int StreamingReceiver::compute_payload_symbol_count(const HeaderDecodeResult &header, bool ldro_enabled) const {
    // Delegate to the payload decoder which encapsulates CR/SF/LDRO specifics.
    return payload_decoder_.compute_payload_symbol_count(header, ldro_enabled);
}

void StreamingReceiver::finalize_frame(std::size_t samples_consumed) {
#ifndef NDEBUG
    std::fprintf(stderr,
                 "[stream] finalizing frame: consume=%zu capture_before=%zu\n",
                 samples_consumed,
                 capture_.size());
#endif
    // Inform the synchronizer to drop the consumed prefix and adjust global offsets.
    synchronizer_.consume(samples_consumed);
    capture_.erase(capture_.begin(), capture_.begin() + static_cast<std::ptrdiff_t>(samples_consumed));
    capture_global_offset_ += samples_consumed;
#ifndef NDEBUG
    std::fprintf(stderr,
                 "[stream] capture_after=%zu global_offset=%zu\n",
                 capture_.size(),
                 capture_global_offset_);
#endif
}

FrameSyncResult StreamingReceiver::make_local_sync(const FrameSyncResult &sync, std::size_t preamble_offset) const {
    // Convert a stream-global sync result into coordinates local to `capture_`
    // starting at the detected preamble; header/payload decoders operate in this space.
    FrameSyncResult local = sync;
    local.preamble_offset = 0;
    local.p_ofs_est -= static_cast<std::ptrdiff_t>(preamble_offset);
#ifndef NDEBUG
    std::fprintf(stderr,
                 "[stream] make_local_sync: original_p_ofs=%td adjusted=%td preamble_offset=%zu\n",
                 sync.p_ofs_est,
                 local.p_ofs_est,
                 preamble_offset);
#endif
    return local;
}

} // namespace lora
