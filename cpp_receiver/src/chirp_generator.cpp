#include "chirp_generator.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

// Chirp synthesis is shared by multiple stages (preamble detection, frame sync,
// header/payload demodulation). This file keeps the math explicit so any change
// in phase model or oversampling assumptions is easy to audit. The helpers return
// `std::complex<double>` sequences that downstream code later casts to float.

namespace lora {

namespace {

// Generate a single LoRa baseband chirp (complex exponential) at baseband.
// - For an up-chirp, instantaneous frequency sweeps linearly from -BW/2 to +BW/2 over one symbol.
// - For a down-chirp, it sweeps from +BW/2 down to -BW/2 (i.e., the complex conjugate time-reversed phase slope).
// Inputs:
//   sf            - Spreading Factor (5..12). Determines chips per symbol: 2^sf.
//   bandwidth_hz  - LoRa bandwidth in Hz (e.g., 125000).
//   sample_rate_hz- Sample rate in Hz. Must be an integer multiple of bandwidth (integer oversampling).
//   up            - true for up-chirp, false for down-chirp.
// Output:
//   Vector of length sps = (2^sf) * (sample_rate_hz / bandwidth_hz) containing e^{j*phase[n]}.
// Notes:
//   - chips_per_symbol = 2^sf, symbol duration T = chips_per_symbol / BW seconds.
//   - Baseband instantaneous frequency is a linear ramp; the phase is the integral of frequency over time.
//   - Phase(t) = 2π*(-BW/2)*t + π*(BW/T)*t^2 for up-chirp; negated for down-chirp.
std::vector<std::complex<double>> make_chirp(int sf, int bandwidth_hz, int sample_rate_hz, bool up) {
    // Validate parameters to avoid undefined behavior later on.
    if (sf < 5 || sf > 12) {
        throw std::invalid_argument("Spreading factor out of supported range (5-12)");
    }
    if (bandwidth_hz <= 0 || sample_rate_hz <= 0) {
        throw std::invalid_argument("Bandwidth and sample rate must be positive");
    }
    if (sample_rate_hz % bandwidth_hz != 0) {
        // We require integer oversampling so that an integer number of samples maps to each LoRa chip.
        throw std::invalid_argument("Sample rate must be an integer multiple of bandwidth for integer oversampling");
    }

    // Oversampling factor (samples per Hz of bandwidth) and symbol sizing
    const std::size_t os_factor = static_cast<std::size_t>(sample_rate_hz) / static_cast<std::size_t>(bandwidth_hz);
    const std::size_t chips_per_symbol = static_cast<std::size_t>(1) << sf; // 2^sf chips per symbol
    const std::size_t sps = chips_per_symbol * os_factor;                    // samples per symbol

    // Scalar doubles for continuous-time calculations
    const double fs = static_cast<double>(sample_rate_hz); // sample rate
    const double bw = static_cast<double>(bandwidth_hz);   // bandwidth
    const double T = static_cast<double>(chips_per_symbol) / bw; // symbol duration in seconds

    std::vector<std::complex<double>> chirp(sps);
    for (std::size_t n = 0; n < sps; ++n) {
        // Discrete time for sample n
        const double t = static_cast<double>(n) / fs;

        // Phase model for a linear frequency sweep centered at DC:
        //   f(t) = -BW/2 + (BW/T)*t  ->  φ(t) = 2π∫f(t)dt = 2π*(-BW/2)*t + π*(BW/T)*t^2
        const double base_phase = 2.0 * std::numbers::pi * (-bw / 2.0) * t
                                + std::numbers::pi * (bw / T) * t * t;

        // For a down-chirp we negate the phase slope, equivalent to time-reversing the sweep direction.
        const double phase = up ? base_phase : -base_phase;

        // e^{j*phase} = cos(phase) + j*sin(phase)
        chirp[n] = std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return chirp;
}

} // namespace

// Convenience wrapper to build an up-chirp with given LoRa parameters.
std::vector<std::complex<double>> make_upchirp(int sf, int bandwidth_hz, int sample_rate_hz) {
    return make_chirp(sf, bandwidth_hz, sample_rate_hz, /*up=*/true);
}

// Convenience wrapper to build a down-chirp with given LoRa parameters.
std::vector<std::complex<double>> make_downchirp(int sf, int bandwidth_hz, int sample_rate_hz) {
    return make_chirp(sf, bandwidth_hz, sample_rate_hz, /*up=*/false);
}

} // namespace lora
