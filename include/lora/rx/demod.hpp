#pragma once
#include <complex>
#include <cstdint>
#include "lora/workspace.hpp"

namespace lora::rx {

// Inline helper: dechirp + FFT + pick max bin
inline uint32_t demod_symbol_peak(Workspace& ws, const std::complex<float>* block) {
    uint32_t N = ws.N;
    for (uint32_t n = 0; n < N; ++n)
        ws.rxbuf[n] = block[n] * ws.downchirp[n];
    ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
    uint32_t max_bin = 0; float max_mag = 0.f;
    for (uint32_t k = 0; k < N; ++k) {
        float mag = std::norm(ws.fftbuf[k]);
        if (mag > max_mag) { max_mag = mag; max_bin = k; }
    }
    return max_bin;
}

} // namespace lora::rx

