#pragma once

#include <complex>
#include <vector>

namespace lora {

std::vector<std::complex<double>> make_upchirp(int sf, int bandwidth_hz, int sample_rate_hz);
std::vector<std::complex<double>> make_downchirp(int sf, int bandwidth_hz, int sample_rate_hz);

} // namespace lora
