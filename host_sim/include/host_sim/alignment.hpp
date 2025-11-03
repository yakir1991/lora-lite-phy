#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace host_sim
{

class FftDemodulator;

std::size_t find_symbol_alignment(const std::vector<std::complex<float>>& samples,
                                  const FftDemodulator& demod,
                                  int preamble_symbols = 8);

} // namespace host_sim
