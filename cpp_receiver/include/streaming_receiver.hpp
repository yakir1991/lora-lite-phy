#pragma once

// Core building blocks used by the streaming receiver implementation
#include "frame_sync.hpp"
#include "header_decoder.hpp"
#include "iq_loader.hpp"
#include "payload_decoder.hpp"
#include "receiver.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lora {

// `StreamingReceiver` maintains enough rolling state to decode LoRa frames while
// ingesting samples in arbitrarily sized chunks. It bridges the existing batch
// primitives (frame synchronizer, header/payload decoders) with buffering logic,
// and emits high-level `FrameEvent`s so applications can observe sync, header
// metadata, incremental payload bytes, and final success/error outcomes.
class StreamingReceiver {
public:
    // Complex baseband sample type forwarded from the IQ loader
    using Sample = IqLoader::Sample;

    // A single, discrete event describing what happened during streaming decode
    // of the most recently pushed samples. Only the fields that are relevant to
    // the specific event type will be populated.
    struct FrameEvent {
        // Event category raised by `push_samples()`
        enum class Type {
            // A new frame's preamble/sync has been detected
            SyncAcquired,
            // The frame header has been decoded (metadata available)
            HeaderDecoded,
            // One more payload byte is available (streamed incrementally)
            PayloadByte,
            // The frame completed (CRC/consistency checked) successfully
            FrameDone,
            // The frame terminated due to an error (sync lost, CRC fail, etc.)
            FrameError,
        } type = Type::SyncAcquired;

        // Absolute sample index in the input stream at which the event is
        // considered to occur (monotonic across `push_samples` calls)
        std::size_t global_sample_index = 0;

        // Present when type == SyncAcquired (localized sync information)
        std::optional<FrameSyncResult> sync;

        // Present when type == HeaderDecoded (decoded header fields)
        std::optional<HeaderDecodeResult> header;

        // Present when type == FrameDone or FrameError (final outcome)
        std::optional<DecodeResult> result;

        // Present when type == PayloadByte (the next decoded payload byte)
        std::optional<unsigned char> payload_byte;

        // Optional human-readable detail (e.g., error description)
        // Only populated for diagnostic or error events.
        std::string message;
    };

    // Construct a streaming receiver from static decoding parameters.
    // The parameters are forwarded to the synchronizer/decoders as needed.
    explicit StreamingReceiver(const DecodeParams &params);

    // Feed an arbitrary chunk of contiguous samples. The method advances
    // internal state, decodes any frames that are now complete or partially
    // available, and returns a sequence of `FrameEvent`s describing observations
    // (sync acquisition, header metadata, streaming payload bytes, and final
    // completion or error). This function is side-effect free beyond internal
    // state updates: it never owns the input memory and always copies what it
    // needs into an internal rolling buffer.
    [[nodiscard]] std::vector<FrameEvent> push_samples(std::span<const Sample> chunk);

    // Clear all rolling state and buffered samples. Useful when changing
    // channels, parameters, or upon hard errors.
    void reset();

private:
    // Immutable decode configuration (SF, BW, CR, LDRO, etc.)
    DecodeParams params_{};

    // Streaming frame synchronizer that can lock to preamble/sync words
    StreamingFrameSynchronizer synchronizer_;

    // Header decoder used after sync acquisition to read frame metadata
    HeaderDecoder header_decoder_;

    // Payload decoder used after a valid header to extract payload bytes and
    // validate CRC/parity constraints
    PayloadDecoder payload_decoder_;

    // Rolling capture buffer of recent input samples from `push_samples`
    std::vector<Sample> capture_;

    // Absolute stream index corresponding to capture_[0]
    std::size_t capture_global_offset_ = 0;

    // Book-keeping for the frame currently being assembled/decoded across
    // multiple calls to `push_samples`
    struct PendingFrame {
        // Synchronization information from the synchronizer
        FrameSyncResult sync{};

        // Offset inside `capture_` where the preamble for this frame begins
        std::size_t preamble_offset = 0;

        // Absolute sample index of the start of this frame's preamble
        std::size_t global_sample_index = 0;

        // Whether we've already emitted the SyncAcquired event for this frame
        bool sync_reported = false;

        // Decoded header (present after header is available and valid)
        std::optional<HeaderDecodeResult> header;

        // Whether we've already emitted the HeaderDecoded event
        bool header_reported = false;

        // Additional samples still required to continue header/payload decode
        std::size_t samples_needed = 0;

        // Effective LDRO setting to use for this frame's payload decode.
        bool ldro_enabled = false;
    };

    // The current frame under construction (if any)
    std::optional<PendingFrame> pending_;

    // Cached convenience: samples per symbol under current params
    std::size_t sps_ = 0;

    // True if enough samples exist in `buffer` to decode the header for `frame`
    [[nodiscard]] bool header_ready(const PendingFrame &frame, const std::vector<Sample> &buffer) const;

    // True if enough samples exist in `buffer` to decode the payload for `frame`
    [[nodiscard]] bool payload_ready(const PendingFrame &frame, const std::vector<Sample> &buffer) const;

    // Offset (in samples from preamble start) where header begins
    [[nodiscard]] std::size_t header_offset_samples() const;

    // Offset (in samples from preamble start) where payload begins
    [[nodiscard]] std::size_t payload_offset_samples() const;

    // Compute number of LoRa symbols the payload spans, given the decoded header
    [[nodiscard]] int compute_payload_symbol_count(const HeaderDecodeResult &header, bool ldro_enabled) const;

    // Finish the current frame, emit terminal event, and advance the capture
    // buffer by `samples_consumed` samples past the frame end
    void finalize_frame(std::size_t samples_consumed);

    // Produce a `FrameSyncResult` whose offsets are local to the current
    // capture buffer (derived from a global/stream-aligned sync result)
    FrameSyncResult make_local_sync(const FrameSyncResult &sync, std::size_t preamble_offset) const;

    [[nodiscard]] bool determine_ldro(const HeaderDecodeResult &header) const;
};

} // namespace lora
