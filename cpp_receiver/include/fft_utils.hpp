#pragma once

#include <complex>
#include <vector>

namespace lora::fft {

// In-place radix-2 Cooley-Tukey FFT for power-of-two sized vectors.
// Matches the sign convention used throughout the receiver:
//   inverse=false -> standard forward transform (negative angle)
//   inverse=true  -> inverse transform without 1/N scaling.
void transform_pow2(std::vector<std::complex<double>> &data, bool inverse);

} // namespace lora::fft

