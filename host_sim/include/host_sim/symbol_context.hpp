#pragma once

#include <complex>
#include <cstdint>
#include <cstddef>
#include <span>

namespace host_sim
{

struct SymbolContext
{
    std::size_t symbol_index{0};
    std::span<const std::complex<float>> samples;
    uint16_t demod_symbol{0};
    bool has_demod_symbol{false};
    double stage_elapsed_ns{0.0};
};

} // namespace host_sim
