#pragma once

#include <complex>
#include <vector>

namespace lora {

// Generate a baseband LoRa upchirp (linear frequency increase over one symbol).
// Parameters:
//  - sf: Spreading factor (5..12). Controls chips per symbol (K = 2^SF).
//  - bandwidth_hz: Signal bandwidth in Hz. Must be positive.
//  - sample_rate_hz: Complex sample rate in Hz. Must be an integer multiple of bandwidth
//    so that oversampling = Fs/BW is an integer (required by the demod path).
// Returns:
//  - A vector of complex<double> samples of length K * oversampling, representing one
//    full symbol duration of the ideal upchirp at baseband, unit amplitude.
std::vector<std::complex<double>> make_upchirp(int sf, int bandwidth_hz, int sample_rate_hz);

// Generate a baseband LoRa downchirp (linear frequency decrease over one symbol).
// Parameters are identical to make_upchirp. The output length and scale are the same,
// but the instantaneous frequency sweeps in the opposite direction. This is used for
// dechirping (matched filtering) during demodulation.
std::vector<std::complex<double>> make_downchirp(int sf, int bandwidth_hz, int sample_rate_hz);

} // namespace lora
