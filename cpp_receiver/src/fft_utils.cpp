#include "fft_utils.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <unordered_map>

// The FFT module intentionally keeps a single translation unit so that the
// fallback radix-2 implementation remains easy to audit. The entry point
// `transform_pow2` transparently dispatches to Liquid-DSP when available, or to
// the bundled radix-2 Cooley–Tukey path otherwise. We guard every public
// function with explicit input validation (power-of-two lengths) because the LoRa
// demod stages rely on tight invariants and we want failures to be loud.

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

void transform_pow2_radix2_float(std::vector<std::complex<float>> &data, bool inverse) {
    const std::size_t n = data.size();
    if (n <= 1) {
        return;
    }

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

    const float base_angle = (inverse ? 2.0f : -2.0f) * static_cast<float>(std::numbers::pi);
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float angle = base_angle / static_cast<float>(len);
        const std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w{1.0f, 0.0f};
            const std::size_t half = len >> 1;
            for (std::size_t k = 0; k < half; ++k) {
                const auto u = data[i + k];
                const auto v = data[i + k + half] * w;
                data[i + k] = u + v;
                data[i + k + half] = u - v;
                w *= wlen;
            }
        }
    }
}

#ifdef LORA_ENABLE_LIQUID_DSP
struct LiquidPlanEntry {
    bool initialise(std::size_t n, bool inverse) {
        input.resize(n);
        output.resize(n);
        plan = fft_create_plan(static_cast<unsigned int>(n),
                               input.data(),
                               output.data(),
                               inverse ? LIQUID_FFT_BACKWARD : LIQUID_FFT_FORWARD,
                               0);
        if (!plan) {
            input.clear();
            output.clear();
            return false;
        }
        return true;
    }

    ~LiquidPlanEntry() {
        if (plan) {
            fft_destroy_plan(plan);
            plan = nullptr;
        }
    }

    fftplan plan = nullptr;
    std::vector<liquid_float_complex> input;
    std::vector<liquid_float_complex> output;
};

struct LiquidPlanCache {
    LiquidPlanEntry *get(std::size_t n, bool inverse) {
        const std::uint64_t key = (static_cast<std::uint64_t>(n) << 1U) |
                                  static_cast<std::uint64_t>(inverse ? 1U : 0U);
        auto it = entries.find(key);
        if (it != entries.end()) {
            return it->second.get();
        }
        auto entry = std::make_unique<LiquidPlanEntry>();
        if (!entry->initialise(n, inverse)) {
            return nullptr;
        }
        auto *raw = entry.get();
        entries.emplace(key, std::move(entry));
        return raw;
    }

    std::unordered_map<std::uint64_t, std::unique_ptr<LiquidPlanEntry>> entries;
};

thread_local LiquidPlanCache g_liquid_plan_cache;

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

    auto *entry = g_liquid_plan_cache.get(n, inverse);
    if (!entry || !entry->plan) {
        return false;
    }

    const auto *src = data.data();
    auto *dst = entry->input.data();
    for (std::size_t i = 0; i < n; ++i) {
        const auto &value = src[i];
        dst[i] = liquid_float_complex(static_cast<float>(value.real()),
                                      static_cast<float>(value.imag()));
    }

    fft_execute(entry->plan);

    const auto *out_src = entry->output.data();
    auto *dest = data.data();
    for (std::size_t i = 0; i < n; ++i) {
        const auto &value = out_src[i];
        dest[i] = std::complex<double>(static_cast<double>(value.real()),
                                       static_cast<double>(value.imag()));
    }
    return true;
}

bool transform_pow2_liquid_float(std::vector<std::complex<float>> &data, bool inverse) {
    const char *control = std::getenv("LORA_USE_LIQUID");
    if (control && std::strcmp(control, "0") == 0) {
        return false;
    }
    const std::size_t n = data.size();
    if (n == 0) {
        return true;
    }

    auto *entry = g_liquid_plan_cache.get(n, inverse);
    if (!entry || !entry->plan) {
        return false;
    }

    std::memcpy(entry->input.data(), data.data(), n * sizeof(liquid_float_complex));
    fft_execute(entry->plan);
    std::memcpy(data.data(), entry->output.data(), n * sizeof(liquid_float_complex));
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

void transform_pow2(std::vector<std::complex<double>> &data, bool inverse, Scratch &scratch) {
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

    auto &buffer = scratch.ensure_twiddles(n);
    // Copy data into scratch buffer to avoid reallocations in repeated calls.
    buffer.assign(data.begin(), data.end());
    transform_pow2_fallback(buffer, inverse);
    std::copy(buffer.begin(), buffer.end(), data.begin());
}

void transform_pow2(std::vector<std::complex<float>> &data, bool inverse, Scratch &scratch) {
    const std::size_t n = data.size();
    if (n == 0) {
        return;
    }
    if (!is_power_of_two(n)) {
        throw std::invalid_argument("transform_pow2 expects power-of-two length");
    }

#ifdef LORA_ENABLE_LIQUID_DSP
    const bool prefer_fallback_small = n <= 32;
    const char *force_fallback_env = std::getenv("LORA_FORCE_FFT_FALLBACK");
    const bool force_fallback = force_fallback_env && std::strcmp(force_fallback_env, "0") != 0 &&
                                std::strcmp(force_fallback_env, "false") != 0;
    if (!force_fallback && !prefer_fallback_small) {
        if (transform_pow2_liquid_float(data, inverse)) {
            return;
        }
#ifdef LORA_REQUIRE_LIQUID
        throw std::runtime_error("Liquid-DSP required but not available at runtime");
#endif
    }
#endif

    (void)scratch;
    transform_pow2_radix2_float(data, inverse);
}

} // namespace lora::fft
