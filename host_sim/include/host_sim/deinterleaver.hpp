#pragma once

#include <cstdint>
#include <vector>

namespace host_sim
{

struct DeinterleaverConfig
{
    int sf{7};
    int cr{1};
    bool is_header{false};
    bool ldro{false};
};

std::vector<uint8_t> deinterleave(const std::vector<uint16_t>& symbols, const DeinterleaverConfig& cfg, std::size_t& consumed);

} // namespace host_sim
