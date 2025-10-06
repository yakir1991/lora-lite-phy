#pragma once

#include "frame_sync.hpp"
#include "header_decoder.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <optional>
#include <vector>

namespace lora {

struct PayloadDecodeResult {
    // Demodulated payload symbol bins (K-domain), for debugging/analysis.
    std::vector<int> raw_symbols;
    // Decoded payload bytes (message only, without CRC bytes even if present).
    std::vector<unsigned char> bytes;
    bool crc_ok = false;
};

class PayloadDecoder {
public:
    using Sample = std::complex<float>;

    // Construct a LoRa payload decoder with PHY parameters.
    PayloadDecoder(int sf, int bandwidth_hz, int sample_rate_hz);

    // Decode payload symbols using timing/CFO from sync and params from header.
    // Inputs:
    //  - samples: full I/Q capture
    //  - sync: FrameSyncResult with p_ofs_est and cfo_hz for rotation/alignment
    //  - header: decoded header (explicit or implicit) providing length/CR/CRC flag
    //  - ldro_enabled: low data rate optimization state (affects symbol mapping)
    // Output:
    //  - PayloadDecodeResult with raw symbols, decoded bytes, and CRC16 result
    //  - std::nullopt on demod/consistency failure
    [[nodiscard]] std::optional<PayloadDecodeResult> decode(
        const std::vector<Sample> &samples,
        const FrameSyncResult &sync,
        const HeaderDecodeResult &header,
        bool ldro_enabled) const;

    [[nodiscard]] std::size_t payload_symbol_offset_samples(bool implicit_header) const;
    [[nodiscard]] int compute_payload_symbol_count(const HeaderDecodeResult &header, bool ldro_enabled) const;

    struct WhiteningMode {
        enum Type { TABLE, PN9, PN9_MSB } type;
        int seed;
    };

private:
    int sf_ = 7;
    int bandwidth_hz_ = 125000;
    int sample_rate_hz_ = 500000;
    std::size_t os_factor_ = 4;
    std::size_t sps_ = 0;

    std::vector<std::complex<double>> downchirp_;

    // Build Gray de-mapping table for a given number of bits per symbol (SF - 2*DE).
    [[nodiscard]] std::vector<int> lora_degray_table(int bits) const;
    // Convert an integer to a vector of bits (LSB-first), length bit_count.
    [[nodiscard]] std::vector<int> num_to_bits(unsigned value, int bit_count) const;
    // Read one byte from a bit vector starting at offset (LSB-first).
    [[nodiscard]] unsigned byte_from_bits(const std::vector<int> &bits, std::size_t offset) const;
    // Remove LoRa payload whitening using the configured mode.
    [[nodiscard]] std::vector<int> dewhiten_bits(const std::vector<int> &bits) const;
    // Compute CRC16 over the first bit_count bits; returns 16 bits LSB-first.
    [[nodiscard]] std::array<int, 16> crc16_bits(const std::vector<int> &bits, std::size_t bit_count) const;

    
    // Attempt decoding with a given set of disambiguation parameters.
    // Used to try alternative whitening modes, bit slides, or start offsets.
    [[nodiscard]] std::optional<std::vector<unsigned char>> try_decode_with_params(
        const std::vector<int> &data_bits,
        const HeaderDecodeResult &header,
        const WhiteningMode &wmode,
        int bit_slide,
        int start_byte) const;
};

} // namespace lora
