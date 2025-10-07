#pragma once

#include "frame_sync.hpp"
#include "header_decoder.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace lora {

struct PayloadDecodeResult {
    // Demodulated payload symbol bins (K-domain), for debugging/analysis.
    std::vector<int> raw_symbols; // Optional owning storage when spans not provided
    // Decoded payload bytes (message only, without CRC bytes even if present).
    std::vector<unsigned char> bytes; // Optional owning storage when spans not provided
    bool crc_ok = false;
    // Lightweight views exposing either the internal vectors or caller-provided buffers (if supplied).
    std::span<const int> raw_symbol_view;
    std::span<const unsigned char> byte_view;
};

class PayloadDecoder {
public:
    using Sample = std::complex<float>;

    // Mutable spans that allow firmware builds to provide preallocated buffers and avoid heap use.
    struct MutableByteSpan {
        unsigned char *data = nullptr;
        std::size_t capacity = 0;
        constexpr MutableByteSpan() = default;
        constexpr MutableByteSpan(unsigned char *ptr, std::size_t cap) : data(ptr), capacity(cap) {}
    };

    struct MutableIntSpan {
        int *data = nullptr;
        std::size_t capacity = 0;
        constexpr MutableIntSpan() = default;
        constexpr MutableIntSpan(int *ptr, std::size_t cap) : data(ptr), capacity(cap) {}
    };

    // Construct a LoRa payload decoder with PHY parameters.
    PayloadDecoder(int sf, int bandwidth_hz, int sample_rate_hz);

    // Decode payload symbols using timing/CFO from sync and params from header.
    // Inputs:
    //  - samples: full I/Q capture
    //  - sync: FrameSyncResult with p_ofs_est and cfo_hz for rotation/alignment
    //  - header: decoded header (explicit or implicit) providing length/CR/CRC flag
    //  - ldro_enabled: low data rate optimization state (affects symbol mapping)
    // Optional inputs (embedded builds):
    //  - external_payload: caller-managed buffer for decoded payload bytes
    //  - external_raw_symbols: caller-managed buffer for raw symbol bins
    // Output:
    //  - PayloadDecodeResult with raw symbols, decoded bytes, and CRC16 result
    //  - std::nullopt on demod/consistency failure
    [[nodiscard]] std::optional<PayloadDecodeResult> decode(
        const std::vector<Sample> &samples,
        const FrameSyncResult &sync,
        const HeaderDecodeResult &header,
        bool ldro_enabled) const;

    [[nodiscard]] std::optional<PayloadDecodeResult> decode(
        const std::vector<Sample> &samples,
        const FrameSyncResult &sync,
        const HeaderDecodeResult &header,
        bool ldro_enabled,
        MutableByteSpan external_payload,
        MutableIntSpan external_raw_symbols) const;

    [[nodiscard]] std::size_t payload_symbol_offset_samples(bool implicit_header) const;
    [[nodiscard]] int compute_payload_symbol_count(const HeaderDecodeResult &header, bool ldro_enabled) const;

private:
    using CDouble = std::complex<double>;

    int sf_ = 7;
    int bandwidth_hz_ = 125000;
    int sample_rate_hz_ = 500000;
    std::size_t os_factor_ = 4;
    std::size_t sps_ = 0;

    std::vector<std::complex<double>> downchirp_;

    // Scratch buffers reused across decode invocations to minimise dynamic allocations.
    mutable std::vector<CDouble> symbol_buffer_;        // Holds one CFO-rotated, dechirped symbol at full sample rate.
    mutable std::vector<CDouble> decimated_buffer_;     // Stores one-sample-per-chip view of the current symbol for FFT.
    mutable std::vector<int> block_bits_buffer_;        // Temporary storage for raw bits within a single interleaving block.
    mutable std::vector<int> interleave_buffer_s_;      // Flattened `S` matrix before LoRa interleaver column rotations.
    mutable std::vector<int> interleave_buffer_c_;      // Flattened `C` matrix after rotations and row flips.
    mutable std::vector<int> payload_bits_buffer_;      // Accumulates payload/header bits prior to dewhitening.
    mutable std::vector<unsigned char> byte_buffer_;    // Holds packed payload bytes before copying into the result.
    mutable std::vector<int> degray_table_;             // Cached inverse-Gray lookup table reused across symbols.
    mutable int degray_bits_ = -1;                      // Tracks how many bits the cached degray table represents.

    // Build (and cache) the inverse Gray mapping table for a given number of bits per symbol (SF - 2*DE).
    [[nodiscard]] const std::vector<int> &get_degray_table(int bits) const;
    // Read one byte from a bit vector starting at offset (LSB-first).
    [[nodiscard]] unsigned byte_from_bits(const std::vector<int> &bits, std::size_t offset) const;
    // Remove LoRa payload whitening using the configured mode.
    void dewhiten_bits(std::vector<int> &bits) const;
    // Compute CRC16 over the first bit_count bits; returns 16 bits LSB-first.
    [[nodiscard]] std::array<int, 16> crc16_bits(const std::vector<int> &bits, std::size_t bit_count) const;
};

} // namespace lora
