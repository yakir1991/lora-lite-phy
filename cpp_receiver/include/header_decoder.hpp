#pragma once

#include "frame_sync.hpp"

#include <complex>
#include <cstddef>
#include <optional>
#include <vector>

namespace lora {

struct HeaderDecodeResult {
    std::vector<int> raw_symbols; // k_hdr values (length 8 for SF7 explicit header)
    bool fcs_ok = false;
    int payload_length = -1;
    bool has_crc = false;
    int cr = -1;
    std::vector<int> payload_header_bits; // residual bits feeding into payload decoding (may be empty)
};

class HeaderDecoder {
public:
    using Sample = std::complex<float>;

    HeaderDecoder(int sf, int bandwidth_hz, int sample_rate_hz);

    [[nodiscard]] std::optional<HeaderDecodeResult> decode(const std::vector<Sample> &samples,
                                                           const FrameSyncResult &sync) const;

private:
    int sf_ = 7;
    int bandwidth_hz_ = 125000;
    int sample_rate_hz_ = 500000;
    std::size_t os_factor_ = 4;
    std::size_t sps_ = 0;

    std::vector<std::complex<double>> downchirp_;

    static int compute_header_crc(int n0, int n1, int n2);
};

} // namespace lora
