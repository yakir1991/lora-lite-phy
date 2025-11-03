#pragma once

#include "host_sim/symbol_context.hpp"

namespace host_sim
{

struct StageConfig
{
    int sf{0};
    int bandwidth{0};
    int sample_rate{0};
};

class Stage
{
public:
    virtual ~Stage() = default;

    virtual void reset(const StageConfig& config) = 0;
    virtual void process(SymbolContext& context) = 0;
    virtual void flush() = 0;
};

} // namespace host_sim
