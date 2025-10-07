#include "streaming_receiver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

// The streaming receiver keeps enough rolling state to decode arbitrarily long
// IQ streams while emitting structured events (sync/header/payload bytes).
// Compared to the batch `Receiver`, it needs extra bookkeeping: sliding buffers,
// guarded consumption, and per-frame metadata. The comments here aim to walk
// through each decision point so maintainers can reason about buffer sizes and
// latency trade-offs. Each helper intentionally answers a “why is this safe?”
// question so refactors do not accidentally break synchronizer invariants.

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

    // Bound `capture_` to not exceed the synchronizer's buffer, keeping absolute indices
    // in sync via `capture_global_offset_`.
    if (capture_.size() > buffer_after_size) {
        const std::size_t drop = capture_.size() - buffer_after_size;
        capture_.erase(capture_.begin(), capture_.begin() + static_cast<std::ptrdiff_t>(drop));
        capture_global_offset_ += drop;
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

    // Header stage: wait until enough samples exist from the detected preamble start
    // (including guard for fractional timing) and attempt header decode.
    if (!frame.header && header_ready(frame, capture_)) {
        FrameSyncResult local_sync = make_local_sync(frame.sync, frame.preamble_offset);
#ifndef NDEBUG
        std::fprintf(stderr,
                     "[stream] attempting header decode: preamble_offset=%zu capture_size=%zu p_ofs_est_local=%td\n",
                     frame.preamble_offset,
                     capture_.size(),
                     local_sync.p_ofs_est);
#endif
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
        const auto header = header_decoder_.decode(view, local_sync, header_symbol_span, header_bits_span);
        if (header.has_value()) {
            frame.header = header;
            frame.sync = local_sync;

#ifndef NDEBUG
            std::fprintf(stderr,
                         "[stream] header decoded: payload_len=%d\n",
                         frame.header->payload_length);
#endif

            // Predict the number of payload symbols and compute total samples needed
            // from the preamble start to cover the entire payload window.
            const int payload_syms = compute_payload_symbol_count(*header);
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
        } else {
#ifndef NDEBUG
            std::fprintf(stderr,
                         "[stream] header decode failed at preamble=%zu capture_size=%zu\n",
                         frame.preamble_offset,
                         capture_.size());
#endif
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
    // Header length is 8 symbols in the standard baseband; payload starts after that.
    return header_offset_samples() + 8u * sps_;
}

int StreamingReceiver::compute_payload_symbol_count(const HeaderDecodeResult &header) const {
    // Delegate to the payload decoder which encapsulates CR/SF/LDRO specifics.
    return payload_decoder_.compute_payload_symbol_count(header, params_.ldro_enabled);
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


