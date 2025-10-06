#pragma once

#include <complex>
#include <vector>

namespace lora::fft {

// In-place radix-2 Cooley–Tukey FFT for power-of-two sized vectors.
// Contract:
//  - Input: data.size() must be a power of two (N = 2^k, k>=1).
//  - Operation: transforms in-place; the input buffer is overwritten with the spectrum/time-domain result.
//  - Direction/sign:
//      inverse=false -> forward transform with negative angular frequency.
//      inverse=true  -> inverse transform with positive angular frequency.
//  - Normalization: no implicit 1/N scaling is applied in either direction.
//    If you need a unitary transform, scale explicitly by 1/N after inverse.
//  - Numerical type: complex<double> to reduce round-off in demod steps.
// Notes:
//  - Some call sites may use a separate scratch/workspace to avoid reallocation;
//    this routine itself operates only on the provided buffer.
void transform_pow2(std::vector<std::complex<double>> &data, bool inverse);

} // namespace lora::fft

