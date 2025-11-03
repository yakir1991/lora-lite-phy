#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace host_sim
{

class WhiteningSequencer
{
public:
    void reset() {}

    std::vector<uint8_t> sequence(std::size_t count) const;

    std::vector<uint8_t> apply(const std::vector<uint8_t>& payload) const;

    std::vector<uint8_t> undo(const std::vector<uint8_t>& payload) const;
};

} // namespace host_sim
