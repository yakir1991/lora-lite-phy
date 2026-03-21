#include "host_sim/impairments.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

namespace host_sim
{

namespace
{
constexpr double kPpmScale = 1e-6;
constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

inline double to_linear(double snr_db)
{
    return std::pow(10.0, snr_db / 10.0);
}

std::vector<std::complex<float>> load_waveform(const std::string& path)
{
    if (path.empty()) {
        return {};
    }
    std::filesystem::path p(path);
    std::ifstream file(p, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to load waveform: " + p.string());
    }
    std::vector<std::complex<float>> data;
    std::complex<float> sample;
    while (file.read(reinterpret_cast<char*>(&sample), sizeof(sample))) {
        data.push_back(sample);
    }
    return data;
}
} // namespace

ImpairmentEngine::ImpairmentEngine(const ImpairmentConfig& config,
                                   double sample_rate_hz,
                                   double symbol_period_s,
                                   std::size_t samples_per_symbol)
    : config_(config),
      sample_rate_hz_(sample_rate_hz),
      symbol_period_s_(symbol_period_s),
      samples_per_symbol_(samples_per_symbol),
      rng_(config.seed)
{
    if (samples_per_symbol_ == 0) {
        samples_per_symbol_ = 1;
    }

    if (config_.collision.enabled && !config_.collision.waveform_path.empty()) {
        collision_waveform_ = load_waveform(config_.collision.waveform_path);
        if (collision_waveform_.empty()) {
            throw std::runtime_error("Collision waveform is empty: " + config_.collision.waveform_path);
        }
    }
}

void ImpairmentEngine::apply_capture(std::vector<std::complex<float>>& samples)
{
    if (!config_.enabled()) {
        return;
    }

    std::size_t offset = 0;
    std::size_t symbol_index = 0;
    std::vector<std::complex<float>> buffer(samples_per_symbol_);

    while (offset + samples_per_symbol_ <= samples.size()) {
        std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(samples_per_symbol_),
                    buffer.begin());
        apply_symbol(symbol_index, buffer);
        std::copy(buffer.begin(), buffer.end(),
                  samples.begin() + static_cast<std::ptrdiff_t>(offset));

        offset += samples_per_symbol_;
        ++symbol_index;
        processed_samples_ += samples_per_symbol_;
    }

    const std::size_t remainder = samples.size() - offset;
    if (remainder > 0) {
        buffer.assign(samples.begin() + static_cast<std::ptrdiff_t>(offset), samples.end());
        apply_symbol(symbol_index, buffer);
        std::copy(buffer.begin(), buffer.end(), samples.begin() + static_cast<std::ptrdiff_t>(offset));
        processed_samples_ += remainder;
    }
}

void ImpairmentEngine::apply_symbol(std::size_t symbol_index,
                                    std::vector<std::complex<float>>& symbol)
{
    if (symbol.empty()) {
        return;
    }

    const double symbol_start_time = static_cast<double>(symbol_index) * symbol_period_s_;

    // Apply SFO (stretch/compress the symbol through interpolation).
    if (std::abs(config_.sfo_ppm) > 0.0 || std::abs(config_.sfo_drift_ppm_per_s) > 0.0) {
        const double sfo_ppm = config_.sfo_ppm +
                               config_.sfo_drift_ppm_per_s * symbol_start_time;
        const double scale = 1.0 + sfo_ppm * kPpmScale;
        if (std::abs(scale - 1.0) > 1e-9) {
            std::vector<std::complex<float>> original = symbol;
            const std::size_t N = original.size();
            for (std::size_t i = 0; i < N; ++i) {
                const double src = static_cast<double>(i) * scale;
                const std::size_t idx0 = static_cast<std::size_t>(std::floor(src));
                const std::size_t idx1 = std::min(idx0 + 1, N - 1);
                const double frac = src - static_cast<double>(idx0);
                symbol[i] = original[idx0] * static_cast<float>(1.0 - frac) +
                            original[idx1] * static_cast<float>(frac);
            }
        }
    }

    // Apply CFO as a rotating phasor.
    if (std::abs(config_.cfo_ppm) > 0.0 || std::abs(config_.cfo_drift_ppm_per_s) > 0.0) {
        for (std::size_t i = 0; i < symbol.size(); ++i) {
            const std::size_t global_index = processed_samples_ + i;
            const double time = static_cast<double>(global_index) / sample_rate_hz_;
            const double cfo_ppm = config_.cfo_ppm +
                                   config_.cfo_drift_ppm_per_s * time;
            const double cfo_hz = sample_rate_hz_ * cfo_ppm * kPpmScale;
            const double phase_increment = kTwoPi * cfo_hz / sample_rate_hz_;
            phase_accum_ += phase_increment;
            symbol[i] *= std::complex<float>(std::cos(phase_accum_), std::sin(phase_accum_));
        }
    }

    // Compute signal power after deterministic impairments.
    double signal_power = 0.0;
    for (const auto& sample : symbol) {
        signal_power += static_cast<double>(std::norm(sample));
    }
    signal_power /= static_cast<double>(symbol.size());
    if (signal_power <= 0.0) {
        signal_power = 1e-12;
    }

    auto add_noise = [&](double target_snr_db) {
        const double snr_linear = to_linear(target_snr_db);
        const double noise_variance = signal_power / snr_linear;
        const double sigma = std::sqrt(noise_variance / 2.0);
        for (auto& sample : symbol) {
            const float noise_re = static_cast<float>(sigma * normal_(rng_));
            const float noise_im = static_cast<float>(sigma * normal_(rng_));
            sample += std::complex<float>(noise_re, noise_im);
        }
    };

    if (config_.awgn_enabled) {
        add_noise(config_.awgn_snr_db);
    }

    if (config_.burst.enabled &&
        config_.burst.period_symbols > 0 &&
        config_.burst.duration_symbols > 0) {
        const std::size_t within_period = symbol_index % config_.burst.period_symbols;
        if (within_period < config_.burst.duration_symbols) {
            add_noise(config_.burst.snr_db);
        }
    }

    if (config_.collision.enabled && config_.collision.probability > 0.0 && !collision_waveform_.empty()) {
        if (uniform_(rng_) < config_.collision.probability) {
            for (std::size_t i = 0; i < symbol.size(); ++i) {
                const std::size_t idx = (collision_offset_ + i) % collision_waveform_.size();
                symbol[i] += static_cast<float>(config_.collision.scale) * collision_waveform_[idx];
            }
            collision_offset_ = (collision_offset_ + symbol.size()) % collision_waveform_.size();
        }
    }
}

} // namespace host_sim
