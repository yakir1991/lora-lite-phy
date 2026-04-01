#pragma once

#include <complex>
#include <filesystem>
#include <vector>

namespace host_sim
{

std::vector<std::complex<float>> load_cf32(const std::filesystem::path& file_path);

/// Read complex-float32 IQ samples from stdin until EOF.
std::vector<std::complex<float>> load_cf32_stdin();

/// Read HackRF-native signed-int8 IQ from stdin and convert to complex float32.
std::vector<std::complex<float>> load_hackrf_stdin();

struct CaptureStats
{
    std::size_t sample_count{0};
    float min_magnitude{0.0F};
    float max_magnitude{0.0F};
    float mean_power{0.0F};
};

CaptureStats analyse_capture(const std::vector<std::complex<float>>& samples);

} // namespace host_sim
