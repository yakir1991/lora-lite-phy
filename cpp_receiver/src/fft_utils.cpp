#include "fft_utils.hpp"

#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

#ifdef LORA_ENABLE_LIQUID_DSP
#include <liquid/liquid.h>
#endif

namespace lora::fft {

namespace {

// Return true iff n is a power of two (and non-zero).
// Used to guard the radix-2 Cooley–Tukey implementation below.
bool is_power_of_two(std::size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

// In-place radix-2 Cooley–Tukey FFT for power-of-two lengths.
// - Implements iterative breadth-first butterflies with bit-reversed input ordering.
// - Supports both forward (inverse=false) and inverse (inverse=true) transforms.
// - No normalization is applied here; caller is responsible if unitary scaling is desired.
void transform_pow2_fallback(std::vector<std::complex<double>> &data, bool inverse) {
    const std::size_t n = data.size();

    // Permute into bit-reversed order so butterflies operate on contiguous blocks.
    std::size_t j = 0;
    for (std::size_t i = 1; i < n; ++i) {
        std::size_t bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    // Twiddle factor base angle: -2π/len for forward, +2π/len for inverse.
    const double base_angle = (inverse ? 2.0 : -2.0) * std::numbers::pi;
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = base_angle / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w{1.0, 0.0};
            const std::size_t half = len >> 1;
            for (std::size_t k = 0; k < half; ++k) {
                // Butterfly: [u, v] -> [u+v, u-v], with v multiplied by current twiddle w.
                const auto u = data[i + k];
                const auto v = data[i + k + half] * w;
                data[i + k] = u + v;
                data[i + k + half] = u - v;
                w *= wlen; // advance twiddle for next butterfly in this group
            }
        }
    }
}

#ifdef LORA_ENABLE_LIQUID_DSP
// Optional acceleration path using Liquid-DSP when compiled with LORA_ENABLE_LIQUID_DSP.
// Controlled by env var LORA_USE_LIQUID (set to "0" to disable at runtime).
// Falls back to the builtin implementation if planning/execution fails.
bool transform_pow2_liquid(std::vector<std::complex<double>> &data, bool inverse) {
    const char *control = std::getenv("LORA_USE_LIQUID");
    if (control && std::strcmp(control, "0") == 0) {
        return false;
    }
    const std::size_t n = data.size();
    if (n == 0) {
        return true;
    }

    // Liquid-DSP uses single-precision complex types; we convert back and forth.
    std::vector<liquid_float_complex> work(n);
    std::vector<liquid_float_complex> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        work[i] = liquid_float_complex(static_cast<float>(data[i].real()),
                                       static_cast<float>(data[i].imag()));
    }

    const int direction = inverse ? LIQUID_FFT_BACKWARD : LIQUID_FFT_FORWARD;
    fftplan plan = fft_create_plan(static_cast<unsigned int>(n),
                                   work.data(),
                                   out.data(),
                                   direction,
                                   0);
    if (!plan) {
        return false;
    }

    fft_execute(plan);
    fft_destroy_plan(plan);

    for (std::size_t i = 0; i < n; ++i) {
        data[i] = std::complex<double>(static_cast<double>(out[i].real()),
                                       static_cast<double>(out[i].imag()));
    }
    return true;
}
#endif

} // namespace

// Public entry point: in-place power-of-two FFT/IFFT.
// - Expects data.size() to be a power of two; throws otherwise.
// - If Liquid-DSP is enabled and available at runtime, uses it; otherwise uses the fallback.
// - No scaling is performed for inverse; apply 1/N externally if you need a unitary transform.
void transform_pow2(std::vector<std::complex<double>> &data, bool inverse) {
    const std::size_t n = data.size();
    if (n == 0) {
        return;
    }
    if (!is_power_of_two(n)) {
        throw std::invalid_argument("transform_pow2 expects power-of-two length");
    }

#ifdef LORA_ENABLE_LIQUID_DSP
    if (transform_pow2_liquid(data, inverse)) {
        return;
    }
#ifdef LORA_REQUIRE_LIQUID
    throw std::runtime_error("Liquid-DSP required but not available at runtime");
#endif
#endif

    transform_pow2_fallback(data, inverse);
}

} // namespace lora::fft
