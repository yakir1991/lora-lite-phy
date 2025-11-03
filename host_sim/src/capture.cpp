#include "host_sim/capture.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace host_sim
{

std::vector<std::complex<float>> load_cf32(const std::filesystem::path& file_path)
{
    if (!std::filesystem::exists(file_path)) {
        throw std::runtime_error("CF32 file not found: " + file_path.string());
    }

    const auto file_size = std::filesystem::file_size(file_path);
    if (file_size % (sizeof(float) * 2) != 0) {
        throw std::runtime_error("CF32 file size is not a multiple of complex float width");
    }

    std::vector<std::complex<float>> samples(file_size / (sizeof(float) * 2));
    std::ifstream input(file_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open CF32 file for reading: " + file_path.string());
    }

    input.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(file_size));
    return samples;
}

CaptureStats analyse_capture(const std::vector<std::complex<float>>& samples)
{
    CaptureStats stats;
    stats.sample_count = samples.size();

    if (samples.empty()) {
        return stats;
    }

    float min_mag = std::numeric_limits<float>::max();
    float max_mag = 0.0F;
    long double power_acc = 0.0L;

    for (const auto& sample : samples) {
        const float magnitude = std::abs(sample);
        min_mag = std::min(min_mag, magnitude);
        max_mag = std::max(max_mag, magnitude);
        power_acc += static_cast<long double>(magnitude) * static_cast<long double>(magnitude);
    }

    stats.min_magnitude = min_mag;
    stats.max_magnitude = max_mag;
    stats.mean_power = static_cast<float>(power_acc / static_cast<long double>(samples.size()));
    return stats;
}

} // namespace host_sim
