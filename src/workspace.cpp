#include "lora/workspace.hpp"
#include <algorithm>
#include <cmath>

namespace lora {

Workspace::~Workspace() {
    if (plan)
        fft_destroy_plan(plan);
}

void Workspace::init(uint32_t new_sf) {
    if (sf == new_sf && N != 0)
        return;
    sf = new_sf;
    N  = 1u << sf;

    if (plan)
        fft_destroy_plan(plan);

    // Buffers and chirps
    upchirp.resize(N);
    downchirp.resize(N);
    rxbuf.resize(N);
    fftbuf.resize(N);
    plan = fft_create_plan(N,
                           reinterpret_cast<liquid_float_complex*>(rxbuf.data()),
                           reinterpret_cast<liquid_float_complex*>(fftbuf.data()),
                           LIQUID_FFT_FORWARD,
                           0);
    for (uint32_t n = 0; n < N; ++n) {
        float phase = static_cast<float>(M_PI) * n * n / N;
        upchirp[n]   = std::exp(std::complex<float>(0.0f, phase));
        downchirp[n] = std::conj(upchirp[n]);
    }
}

void Workspace::ensure_rx_buffers(size_t nsym, uint32_t sf, uint32_t cr_plus4) {
    size_t bits_needed    = nsym * sf;
    size_t nibbles_needed = bits_needed / cr_plus4;
    size_t data_needed    = (nibbles_needed + 1) / 2;
    if (rx_symbols.size() < nsym)
        rx_symbols.resize(nsym);
    if (rx_bits.size() < bits_needed)
        rx_bits.resize(bits_needed);
    if (rx_deint.size() < bits_needed)
        rx_deint.resize(bits_needed);
    if (rx_nibbles.size() < nibbles_needed)
        rx_nibbles.resize(nibbles_needed);
    if (rx_data.size() < data_needed)
        rx_data.resize(data_needed);
}

void Workspace::ensure_tx_buffers(size_t payload_len, uint32_t sf, uint32_t cr_plus4) {
    size_t total_bytes = payload_len + 2; // payload + CRC
    size_t bits_needed = total_bytes * 2 * cr_plus4;
    uint32_t block_bits = sf * cr_plus4;
    if (bits_needed % block_bits)
        bits_needed = ((bits_needed / block_bits) + 1) * block_bits;
    size_t nsym = bits_needed / sf;
    if (tx_bits.size() < bits_needed)
        tx_bits.resize(bits_needed);
    if (tx_inter.size() < bits_needed)
        tx_inter.resize(bits_needed);
    if (tx_symbols.size() < nsym)
        tx_symbols.resize(nsym);
    if (tx_iq.size() < nsym * N)
        tx_iq.resize(nsym * N);
}

void Workspace::fft(const std::complex<float>* in, std::complex<float>* out) {
    if (in != rxbuf.data())
        std::copy(in, in + N, rxbuf.begin());
    fft_execute(plan);
    if (out != fftbuf.data())
        std::copy(fftbuf.begin(), fftbuf.begin() + N, out);
}

} // namespace lora

