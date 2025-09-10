#include "lora/rx/decimate.hpp"
#include <liquid/liquid.h>
#include <algorithm>
#include <stdexcept>

namespace lora::rx {

std::vector<std::complex<float>> decimate_os_phase(std::span<const std::complex<float>> x,
                                                   int os,
                                                   int phase,
                                                   float as_db) {
    if (os <= 1) {
        // No decimation; optionally apply phase offset by dropping first 'phase' samples
        size_t start = (phase > 0 && static_cast<size_t>(phase) < x.size()) ? static_cast<size_t>(phase) : 0u;
        return std::vector<std::complex<float>>(x.begin() + start, x.end());
    }
    if (phase < 0 || phase >= os) phase = ((phase % os) + os) % os;
    // Design Kaiser low-pass for integer decimator
    const unsigned int M = static_cast<unsigned int>(os);
    const float fc = 0.45f / static_cast<float>(os); // normalized to fs/2
    const unsigned int L = std::max<unsigned int>(32u * M, 8u * M); // tap count
    std::vector<float> h(L);
    liquid_firdes_kaiser(L, fc, as_db, 0.0f, h.data());
    // Create decimator
    firdecim_crcf q = firdecim_crcf_create(M, h.data(), L);
    if (!q) throw std::runtime_error("firdecim_crcf_create failed");
    // Execute on phase-shifted input
    size_t n_in = x.size();
    if (static_cast<size_t>(phase) >= n_in) {
        firdecim_crcf_destroy(q);
        return {};
    }
    const std::complex<float>* in = x.data() + phase;
    size_t usable = ((n_in - static_cast<size_t>(phase)) / os) * os;
    size_t n_out = usable / os;
    std::vector<std::complex<float>> y;
    y.resize(n_out);
    // Process in blocks of M samples
    size_t yo = 0;
    for (size_t i = 0; i < usable; i += os) {
        liquid_float_complex out;
        firdecim_crcf_execute(q, reinterpret_cast<liquid_float_complex*>(const_cast<std::complex<float>*>(in + i)), &out);
        y[yo++] = out;
    }
    firdecim_crcf_destroy(q);
    return y;
}

} // namespace lora::rx
