#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <liquid/liquid.h>

namespace lora {

class Workspace {
public:
    Workspace();
    ~Workspace();

    void init(uint32_t new_sf);
    void reset_debug_variables();
    void allocate_buffers();
    void create_fft_plan();
    void generate_chirps();
    void fft(const std::complex<float>* in, std::complex<float>* out);

    uint32_t sf{0};
    uint32_t N{0};

    std::vector<std::complex<float>> upchirp;
    std::vector<std::complex<float>> downchirp;
    std::vector<std::complex<float>> rxbuf;
    std::vector<std::complex<float>> fftbuf;

    bool dbg_hdr_filled{false};
    uint32_t dbg_hdr_sf{0};
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
    int dbg_hdr_cfo_int{0};
    std::array<uint32_t, 16> dbg_hdr_syms_raw{};
    std::array<uint32_t, 16> dbg_hdr_syms_corr{};
    std::array<uint32_t, 16> dbg_hdr_gray{};
    std::array<uint8_t, 10> dbg_hdr_nibbles_cr48{};
    std::array<uint8_t, 10> dbg_hdr_nibbles_cr45{};

private:
    fftplan plan{nullptr};
};

} // namespace lora
