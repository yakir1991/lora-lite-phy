#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace host_sim
{

struct ImpairmentConfig
{
    double cfo_ppm{0.0};
    double cfo_drift_ppm_per_s{0.0};
    double sfo_ppm{0.0};
    double sfo_drift_ppm_per_s{0.0};
    bool awgn_enabled{false};
    double awgn_snr_db{40.0};

    struct Burst
    {
        bool enabled{false};
        std::size_t period_symbols{0};
        std::size_t duration_symbols{0};
        double snr_db{10.0};
    } burst;

    struct Collision
    {
        bool enabled{false};
        double probability{0.0};
        double scale{1.0};
        std::string waveform_path;
    } collision;

    uint32_t seed{0};

    bool enabled() const
    {
        bool collision_active = collision.enabled && collision.probability > 0.0 && !collision.waveform_path.empty();
        return awgn_enabled || burst.enabled || collision_active ||
               std::abs(cfo_ppm) > 0.0 || std::abs(cfo_drift_ppm_per_s) > 0.0 ||
               std::abs(sfo_ppm) > 0.0 || std::abs(sfo_drift_ppm_per_s) > 0.0;
    }
};

class ImpairmentEngine
{
public:
    ImpairmentEngine(const ImpairmentConfig& config,
                     double sample_rate_hz,
                     double symbol_period_s,
                     std::size_t samples_per_symbol);

    void apply_capture(std::vector<std::complex<float>>& samples);

private:
    void apply_symbol(std::size_t symbol_index,
                      std::vector<std::complex<float>>& symbol);

    ImpairmentConfig config_;
    double sample_rate_hz_;
    double symbol_period_s_;
    std::size_t samples_per_symbol_;

    double phase_accum_{0.0};
    std::size_t processed_samples_{0};

    std::mt19937 rng_;
    std::normal_distribution<double> normal_{0.0, 1.0};
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::vector<std::complex<float>> collision_waveform_;
    std::size_t collision_offset_{0};
};

} // namespace host_sim
