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

    void init(uint32_t new_sf);
    void fft(const std::complex<float>* in, std::complex<float>* out) const;
};

} // namespace lora

