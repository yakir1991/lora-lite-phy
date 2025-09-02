#pragma once
#include <cstdint>
#include <vector>
#include <complex>

namespace lora {

struct FftPlan {
    uint32_t N{};
    uint32_t log2N{};
    std::vector<std::complex<float>> twiddles; // N/2 twiddles
    std::vector<uint32_t> bitrev;               // N indices
};

struct Workspace {
    uint32_t sf{};
    uint32_t N{};
    FftPlan plan;
    std::vector<std::complex<float>> upchirp;
    std::vector<std::complex<float>> downchirp;
    std::vector<std::complex<float>> rxbuf;
    std::vector<std::complex<float>> fftbuf;

    // Scratch buffers for RX path to avoid per-call allocations
    std::vector<uint32_t> rx_symbols;
    std::vector<uint8_t>  rx_bits;
    std::vector<uint8_t>  rx_deint;
    std::vector<uint8_t>  rx_nibbles;
    std::vector<uint8_t>  rx_data;

    void init(uint32_t new_sf);
    void fft(const std::complex<float>* in, std::complex<float>* out) const;
    void ensure_rx_buffers(size_t nsym, uint32_t sf, uint32_t cr_plus4);
};

} // namespace lora

