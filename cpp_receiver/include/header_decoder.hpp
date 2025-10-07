#pragma once

#include "frame_sync.hpp"

#include <complex>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace lora {

struct HeaderDecodeResult {
    // Raw demodulated header symbol bins (K-domain), length equals header symbol count
    // (e.g., 8 for SF7 explicit header). Useful for debugging and parity checks.
    std::vector<int> raw_symbols;
    bool fcs_ok = false;
    // Payload length in bytes as indicated by the header (0..255 for standard LoRa PHY).
    int payload_length = -1;
    // Whether a CRC16 is present for the payload.
    bool has_crc = false;
    // Code rate (CR) decoded from the header, range 1..4.
    int cr = -1;
    // True when operating in implicit-header mode (fields supplied externally).
    bool implicit_header = false;
    // Residual header bits passed along to payload decoding (may be empty).
    std::vector<int> payload_header_bits;
    // Views to reduce copying when callers provide fixed storage.
    std::span<const int> raw_symbol_view;
    std::span<const int> payload_header_bits_view;
};

class HeaderDecoder {
public:
    using Sample = std::complex<float>;

    // Construct a LoRa header decoder with PHY parameters.
    // Parameters:
    //  - sf: Spreading factor (5..12)
    //  - bandwidth_hz: LoRa bandwidth in Hz
    //  - sample_rate_hz: complex sample rate in Hz (must be integer multiple of BW)
    HeaderDecoder(int sf, int bandwidth_hz, int sample_rate_hz);

    // Decode the header symbols starting at the synchronized frame position.
    // Inputs:
    //  - samples: entire I/Q buffer
    //  - sync: result from FrameSynchronizer with timing/CFO
    // Output:
    //  - HeaderDecodeResult with fcs_ok flag, payload length, has_crc, and CR.
    //  - std::nullopt if demodulation fails or an internal consistency check fails.
    [[nodiscard]] std::optional<HeaderDecodeResult> decode(const std::vector<Sample> &samples,
                                                           const FrameSyncResult &sync) const;

    struct MutableIntSpan {
        int *data = nullptr;
        std::size_t capacity = 0;
        constexpr MutableIntSpan() = default;
        constexpr MutableIntSpan(int *ptr, std::size_t cap) : data(ptr), capacity(cap) {}
    };

    [[nodiscard]] std::optional<HeaderDecodeResult> decode(const std::vector<Sample> &samples,
                                                           const FrameSyncResult &sync,
                                                           MutableIntSpan header_symbols,
                                                           MutableIntSpan header_bits) const;

    [[nodiscard]] std::size_t symbol_span_samples() const;

private:
    int sf_ = 7;
    int bandwidth_hz_ = 125000;
    int sample_rate_hz_ = 500000;
    std::size_t os_factor_ = 4;
    std::size_t sps_ = 0;

    std::vector<std::complex<double>> downchirp_;

    // Compute LoRa header CRC5 from the first three header nibbles.
    static int compute_header_crc(int n0, int n1, int n2);
};

} // namespace lora
