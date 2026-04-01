#include "host_sim/capture.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

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

namespace
{

void set_stdin_binary()
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
}

} // namespace

std::vector<std::complex<float>> load_cf32_stdin()
{
    set_stdin_binary();
    constexpr std::size_t chunk_floats = 65536; // 32K complex samples per read
    std::vector<std::complex<float>> samples;
    std::vector<float> buf(chunk_floats);

    while (true) {
        const auto n = std::fread(buf.data(), sizeof(float), chunk_floats, stdin);
        if (n == 0) break;
        // Ensure pairs (drop trailing lone float if any)
        const auto pairs = (n / 2) * 2;
        for (std::size_t i = 0; i < pairs; i += 2) {
            samples.emplace_back(buf[i], buf[i + 1]);
        }
    }
    if (samples.empty()) {
        throw std::runtime_error("No IQ samples read from stdin");
    }
    return samples;
}

std::vector<std::complex<float>> load_hackrf_stdin()
{
    set_stdin_binary();
    constexpr std::size_t chunk_bytes = 131072; // 64K complex samples per read
    std::vector<std::complex<float>> samples;
    std::vector<int8_t> buf(chunk_bytes);

    while (true) {
        const auto n = std::fread(buf.data(), 1, chunk_bytes, stdin);
        if (n == 0) break;
        const auto pairs = (n / 2) * 2;
        for (std::size_t i = 0; i < pairs; i += 2) {
            samples.emplace_back(
                static_cast<float>(buf[i]) / 128.0F,
                static_cast<float>(buf[i + 1]) / 128.0F);
        }
    }
    if (samples.empty()) {
        throw std::runtime_error("No IQ samples read from stdin");
    }
    return samples;
}

} // namespace host_sim
