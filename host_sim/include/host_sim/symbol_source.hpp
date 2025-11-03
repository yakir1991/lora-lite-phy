#pragma once

#include <complex>
#include <optional>
#include <vector>

namespace host_sim
{

struct SymbolBuffer
{
    std::vector<std::complex<float>> samples;
};

class SymbolSource
{
public:
    virtual ~SymbolSource() = default;

    virtual void reset() = 0;
    virtual std::optional<SymbolBuffer> next_symbol() = 0;
};

} // namespace host_sim
