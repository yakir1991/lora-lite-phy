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
    std::vector<int> raw_symbols;       // Demodulated payload symbol bins
    std::vector<unsigned char> bytes;   // Decoded payload bytes (message only)
    bool crc_ok = false;
};

class PayloadDecoder {
public:
    using Sample = std::complex<float>;

    PayloadDecoder(int sf, int bandwidth_hz, int sample_rate_hz);

    [[nodiscard]] std::optional<PayloadDecodeResult> decode(
        const std::vector<Sample> &samples,
        const FrameSyncResult &sync,
        const HeaderDecodeResult &header,
        bool ldro_enabled) const;

private:
    int sf_ = 7;
    int bandwidth_hz_ = 125000;
    int sample_rate_hz_ = 500000;
    std::size_t os_factor_ = 4;
    std::size_t sps_ = 0;

    std::vector<std::complex<double>> downchirp_;

    [[nodiscard]] std::size_t payload_symbol_offset_samples(bool implicit_header) const;
    [[nodiscard]] std::vector<int> lora_degray_table(int bits) const;
    [[nodiscard]] int compute_payload_symbol_count(const HeaderDecodeResult &header, bool ldro_enabled) const;
    [[nodiscard]] std::vector<int> num_to_bits(unsigned value, int bit_count) const;
    [[nodiscard]] unsigned byte_from_bits(const std::vector<int> &bits, std::size_t offset) const;
    [[nodiscard]] std::vector<int> dewhiten_bits(const std::vector<int> &bits) const;
    [[nodiscard]] std::array<int, 16> crc16_bits(const std::vector<int> &bits, std::size_t bit_count) const;
};

} // namespace lora
