#include "lora/workspace.hpp"

#include <algorithm>
#include <cmath>

namespace lora {

Workspace::Workspace() : plan(nullptr) {}

Workspace::~Workspace() {
    if (plan)
        fft_destroy_plan(plan);
}

void Workspace::init(uint32_t new_sf) {
    dbg_hdr_filled = false;
    dbg_hdr_sf = 0;
    std::fill(std::begin(dbg_hdr_syms_raw), std::end(dbg_hdr_syms_raw), 0u);
    std::fill(std::begin(dbg_hdr_syms_corr), std::end(dbg_hdr_syms_corr), 0u);
    std::fill(std::begin(dbg_hdr_gray), std::end(dbg_hdr_gray), 0u);
    std::fill(std::begin(dbg_hdr_nibbles_cr48), std::end(dbg_hdr_nibbles_cr48), 0u);
    std::fill(std::begin(dbg_hdr_nibbles_cr45), std::end(dbg_hdr_nibbles_cr45), 0u);
    dbg_hdr_os = 1;
    dbg_hdr_phase = 0;
    dbg_hdr_det_start_raw = 0;
    dbg_hdr_start_decim = 0;
    dbg_hdr_preamble_start = 0;
    dbg_hdr_aligned_start = 0;
    dbg_hdr_sync_start = 0;
    dbg_hdr_header_start = 0;
    dbg_hdr_cfo = 0.0f;
    dbg_hdr_sto = 0;

    if (sf == new_sf && N != 0)
        return;
    sf = new_sf;
    N = 1u << sf;

    if (plan) {
        fft_destroy_plan(plan);
        plan = nullptr;
    }

    upchirp.resize(N);
    downchirp.resize(N);
    rxbuf.resize(N);
    fftbuf.resize(N);

    plan = fft_create_plan(
        N,
        reinterpret_cast<liquid_float_complex*>(rxbuf.data()),
        reinterpret_cast<liquid_float_complex*>(fftbuf.data()),
        LIQUID_FFT_FORWARD,
        0);

    for (uint32_t n = 0; n < N; ++n) {
        float phase = 2.0f * static_cast<float>(M_PI) * (n * n / (2.0f * N) - 0.5f * n);
        upchirp[n] = std::exp(std::complex<float>(0.0f, phase));
        downchirp[n] = std::conj(upchirp[n]);
    }
}

void Workspace::fft(const std::complex<float>* in, std::complex<float>* out) {
    if (in != rxbuf.data())
        std::copy(in, in + N, rxbuf.begin());
    fft_execute(plan);
    if (out != fftbuf.data())
        std::copy(fftbuf.begin(), fftbuf.begin() + N, out);
}

} // namespace lora
