#include "lora/workspace.hpp"
#include <cmath>

namespace lora {

static uint32_t bit_reverse(uint32_t x, uint32_t nbits) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < nbits; ++i) {
        r = (r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

void Workspace::init(uint32_t new_sf) {
    if (sf == new_sf && N != 0)
        return;
    sf = new_sf;
    N  = 1u << sf;

    // FFT plan
    plan.N      = N;
    plan.log2N  = sf;
    plan.twiddles.resize(N / 2);
    plan.bitrev.resize(N);
    const float two_pi_over_N = -2.0f * static_cast<float>(M_PI) / N;
    for (uint32_t k = 0; k < N / 2; ++k)
        plan.twiddles[k] = std::exp(std::complex<float>(0.0f, two_pi_over_N * k));
    for (uint32_t i = 0; i < N; ++i)
        plan.bitrev[i] = bit_reverse(i, sf);

    // Buffers and chirps
    upchirp.resize(N);
    downchirp.resize(N);
    rxbuf.resize(N);
    fftbuf.resize(N);
    for (uint32_t n = 0; n < N; ++n) {
        float phase = static_cast<float>(M_PI) * n * n / N;
        upchirp[n]   = std::exp(std::complex<float>(0.0f, phase));
        downchirp[n] = std::conj(upchirp[n]);
    }
}

void Workspace::fft(const std::complex<float>* in, std::complex<float>* out) const {
    const uint32_t N = plan.N;
    const uint32_t log2N = plan.log2N;

    // Bit-reverse copy
    for (uint32_t i = 0; i < N; ++i)
        out[plan.bitrev[i]] = in[i];

    // Iterative Cooley-Tukey
    for (uint32_t s = 1; s <= log2N; ++s) {
        uint32_t m  = 1u << s;
        uint32_t m2 = m >> 1;
        uint32_t step = N / m;
        for (uint32_t k = 0; k < N; k += m) {
            for (uint32_t j = 0; j < m2; ++j) {
                auto t = plan.twiddles[j * step] * out[k + j + m2];
                auto u = out[k + j];
                out[k + j]       = u + t;
                out[k + j + m2]  = u - t;
            }
        }
    }
}

} // namespace lora

