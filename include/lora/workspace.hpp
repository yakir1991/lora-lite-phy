#pragma once
#include <cstdint>
#include <vector>
#include <complex>
#include <liquid/liquid.h>

using liquid_fftplan = fftplan;

namespace lora {

struct Workspace {
    uint32_t sf{};
    uint32_t N{};
    liquid_fftplan plan{};
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

    // Scratch buffers for TX path to avoid per-call allocations
    std::vector<uint8_t>  tx_bits;
    std::vector<uint8_t>  tx_inter;
    std::vector<uint32_t> tx_symbols;
    std::vector<std::complex<float>> tx_iq;

    ~Workspace();
    void init(uint32_t new_sf);
    void fft(const std::complex<float>* in, std::complex<float>* out);
    void ensure_rx_buffers(size_t nsym, uint32_t sf, uint32_t cr_plus4);
    void ensure_tx_buffers(size_t payload_len, uint32_t sf, uint32_t cr_plus4);
};

} // namespace lora

