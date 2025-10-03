#include "chirp_generator.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace lora {

namespace {

std::vector<std::complex<double>> make_chirp(int sf, int bandwidth_hz, int sample_rate_hz, bool up) {
    if (sf < 5 || sf > 12) {
        throw std::invalid_argument("Spreading factor out of supported range (5-12)");
    }
    if (bandwidth_hz <= 0 || sample_rate_hz <= 0) {
        throw std::invalid_argument("Bandwidth and sample rate must be positive");
    }
    if (sample_rate_hz % bandwidth_hz != 0) {
        throw std::invalid_argument("Sample rate must be an integer multiple of bandwidth for integer oversampling");
    }

    const std::size_t os_factor = static_cast<std::size_t>(sample_rate_hz) / static_cast<std::size_t>(bandwidth_hz);
    const std::size_t chips_per_symbol = static_cast<std::size_t>(1) << sf;
    const std::size_t sps = chips_per_symbol * os_factor;

    const double fs = static_cast<double>(sample_rate_hz);
    const double bw = static_cast<double>(bandwidth_hz);
    const double T = static_cast<double>(chips_per_symbol) / bw;

    std::vector<std::complex<double>> chirp(sps);
    for (std::size_t n = 0; n < sps; ++n) {
        const double t = static_cast<double>(n) / fs;
        const double base_phase = 2.0 * std::numbers::pi * (-bw / 2.0) * t
                                + std::numbers::pi * (bw / T) * t * t;
        const double phase = up ? base_phase : -base_phase;
        chirp[n] = std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return chirp;
}

} // namespace

std::vector<std::complex<double>> make_upchirp(int sf, int bandwidth_hz, int sample_rate_hz) {
    return make_chirp(sf, bandwidth_hz, sample_rate_hz, /*up=*/true);
}

std::vector<std::complex<double>> make_downchirp(int sf, int bandwidth_hz, int sample_rate_hz) {
    return make_chirp(sf, bandwidth_hz, sample_rate_hz, /*up=*/false);
}

} // namespace lora
