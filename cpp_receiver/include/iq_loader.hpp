#pragma once

#include <complex>
#include <filesystem>
#include <string>
#include <vector>

namespace lora {

class IqLoader {
public:
    using Sample = std::complex<float>;

    static std::vector<Sample> load_cf32(const std::filesystem::path &path);
};

} // namespace lora
