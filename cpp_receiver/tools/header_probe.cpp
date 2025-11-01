#include "header_decoder.hpp"

#include <cstdio>
#include <array>
#include <vector>
#include <optional>

int main() {
    const int sf = 7;
    const int bw = 125000;
    const int fs = 500000;

    lora::HeaderDecoder dec(sf, bw, fs);

    // Sweep a small matrix of cases to validate header decode and checksum across CR/CRC/length.
    struct Case { int len; int cr; bool has_crc; };
    std::vector<Case> cases = {
        {0, 1, false}, {1, 1, true}, {11, 2, true}, {18, 2, true}, {21, 2, false},
        {7, 3, false}, {19, 4, true}, {127, 2, true}
    };

    int failures = 0;
    for (const auto &tc : cases) {
        auto tx_v = lora::HeaderDecoder::encode_low_sf_symbols(sf, tc.cr, tc.has_crc, tc.len);
        std::array<int, 8> raw_k{};
        const int K = 1 << sf;
        for (int i = 0; i < 8; ++i) {
            const int v = tx_v[static_cast<std::size_t>(i)];
            const int g = v ^ (v >> 1);
            raw_k[static_cast<std::size_t>(i)] = (K - 1 - (g << 2)) & (K - 1);
        }
        std::fprintf(stderr, "[case] len=%d cr=%d crc=%d v=", tc.len, tc.cr, tc.has_crc ? 1 : 0);
        for (int i = 0; i < 8; ++i) {
            const int v = tx_v[static_cast<std::size_t>(i)];
            std::fprintf(stderr, "%d%s", v, i + 1 == 8 ? "" : ",");
        }
        std::fprintf(stderr, " raw_k=");
        for (int i = 0; i < 8; ++i) {
            std::fprintf(stderr, "%d%s", raw_k[static_cast<std::size_t>(i)], i + 1 == 8 ? "\n" : ",");
        }
        auto res = dec.decode_from_raw_symbols(raw_k);
        if (!res.has_value()) {
            std::fprintf(stderr, "[FAIL] decode null for len=%d cr=%d crc=%d\n", tc.len, tc.cr, tc.has_crc ? 1 : 0);
            failures++;
            continue;
        }
        const auto &r = *res;
        const bool ok = r.fcs_ok && r.payload_length == tc.len && r.cr == tc.cr && r.has_crc == tc.has_crc;
        std::fprintf(stdout, "len=%3d cr=%d crc=%d -> fcs_ok=%d len=%d cr=%d has_crc=%d%s\n",
                     tc.len, tc.cr, tc.has_crc ? 1 : 0,
                     r.fcs_ok ? 1 : 0, r.payload_length, r.cr, r.has_crc ? 1 : 0,
                     ok ? "" : " [MISMATCH]");
        if (!ok) failures++;
    }
    return failures == 0 ? 0 : 1;
}
