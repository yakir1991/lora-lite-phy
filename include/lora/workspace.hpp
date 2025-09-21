#pragma once

#include <complex>
#include <cstdint>
#include <vector>

#if __has_include(<liquid/liquid.h>)
#include <liquid/liquid.h>
#elif __has_include(<liquid.h>)
#include <liquid.h>
#else
#error "liquid-dsp headers not found; initialise external/liquid-dsp or install the library."
#endif

namespace lora {

using liquid_fftplan = fftplan;

struct Workspace {
    uint32_t sf{};
    uint32_t N{};
    liquid_fftplan plan{};
    std::vector<std::complex<float>> upchirp;
    std::vector<std::complex<float>> downchirp;
    std::vector<std::complex<float>> rxbuf;
    std::vector<std::complex<float>> fftbuf;

    bool dbg_hdr_filled{};
    uint32_t dbg_hdr_sf{};
    uint32_t dbg_hdr_syms_raw[16]{};
    uint32_t dbg_hdr_syms_corr[16]{};
    uint32_t dbg_hdr_gray[16]{};
    uint8_t dbg_hdr_nibbles_cr48[10]{};
    uint8_t dbg_hdr_nibbles_cr45[10]{};
    int dbg_hdr_os{1};
    int dbg_hdr_phase{0};
    size_t dbg_hdr_det_start_raw{0};
    size_t dbg_hdr_start_decim{0};
    size_t dbg_hdr_preamble_start{0};
    size_t dbg_hdr_aligned_start{0};
    size_t dbg_hdr_sync_start{0};
    size_t dbg_hdr_header_start{0};
    float dbg_hdr_cfo{0.0f};
    int dbg_hdr_sto{0};

    Workspace();
    ~Workspace();

    void init(uint32_t new_sf);
    void fft(const std::complex<float>* in, std::complex<float>* out);
};

} // namespace lora
