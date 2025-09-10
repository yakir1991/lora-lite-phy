#pragma once
#include <complex>
#include <span>
#include <vector>

namespace lora::rx {

// FIR polyphase decimation by integer 'os' with optional starting phase.
// Uses a Kaiser low-pass anti-aliasing filter designed for the given factor.
// Returns an OS=1 sequence. Phase in [0, os).
std::vector<std::complex<float>> decimate_os_phase(std::span<const std::complex<float>> x,
                                                   int os,
                                                   int phase = 0,
                                                   float as_db = 60.0f);

} // namespace lora::rx

