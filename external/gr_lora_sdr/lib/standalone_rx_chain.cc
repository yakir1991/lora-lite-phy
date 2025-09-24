/* -*- c++ -*- */
/*
 * Standalone LoRa SDR receive chain for experimental use.
 * This file is part of gr-lora_sdr and distributed under the terms
 * of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any
 * later version.
 */

#include "gnuradio/lora_sdr/standalone_rx_chain.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace {
constexpr double k_pi = 3.141592653589793238462643383279502884;
}

namespace gr {
namespace lora_sdr {
namespace experimental {

vector_source::vector_source() = default;

vector_source::vector_source(complex_vector samples)
    : d_samples(std::move(samples))
{
}

void vector_source::set_samples(const complex_vector& samples)
{
    d_samples = samples;
}

void vector_source::set_samples(complex_vector&& samples) noexcept
{
    d_samples = std::move(samples);
}

const complex_vector& vector_source::samples() const noexcept
{
    return d_samples;
}

bool vector_source::empty() const noexcept
{
    return d_samples.empty();
}

window_block::window_block()
    : d_size(0), d_type(window_type::hann)
{
}

window_block::window_block(size_t size, window_type type)
    : d_size(0), d_type(type)
{
    if (size > 0) {
        set_size(size);
    }
}

size_t window_block::size() const noexcept
{
    return d_size;
}

window_block::window_type window_block::type() const noexcept
{
    return d_type;
}

void window_block::set_size(size_t size)
{
    if (size == 0) {
        d_size = 0;
        d_coeffs.clear();
        return;
    }

    d_size = size;
    d_coeffs.assign(size, 1.0F);
    compute_coefficients();
}

void window_block::set_type(window_type type)
{
    if (d_type != type) {
        d_type = type;
        if (d_size > 0) {
            compute_coefficients();
        }
    }
}

const std::vector<float>& window_block::coefficients() const noexcept
{
    return d_coeffs;
}

void window_block::compute_coefficients()
{
    if (d_size == 0) {
        return;
    }

    if (d_size == 1) {
        d_coeffs[0] = 1.0F;
        return;
    }

    const double n_minus_one = static_cast<double>(d_size - 1U);
    for (size_t n = 0; n < d_size; ++n) {
        const double ratio = static_cast<double>(n) / n_minus_one;
        double value = 1.0;

        switch (d_type) {
        case window_type::rectangular:
            value = 1.0;
            break;
        case window_type::hann:
            value = 0.5 - 0.5 * std::cos(2.0 * k_pi * ratio);
            break;
        case window_type::hamming:
            value = 0.54 - 0.46 * std::cos(2.0 * k_pi * ratio);
            break;
        case window_type::blackmanharris:
            value = 0.35875 - 0.48829 * std::cos(2.0 * k_pi * ratio) +
                    0.14128 * std::cos(4.0 * k_pi * ratio) -
                    0.01168 * std::cos(6.0 * k_pi * ratio);
            break;
        }

        d_coeffs[n] = static_cast<float>(value);
    }
}

complex_vector window_block::process(const complex_vector& input) const
{
    if (d_size == 0) {
        throw std::runtime_error("window_block is not configured");
    }

    if (input.size() != d_size) {
        throw std::invalid_argument("window_block::process size mismatch");
    }

    complex_vector output(d_size);
    for (size_t n = 0; n < d_size; ++n) {
        output[n] = input[n] * d_coeffs[n];
    }

    return output;
}

liquid_fft_block::liquid_fft_block()
    : d_fft_size(0), d_plan(nullptr)
{
}

liquid_fft_block::liquid_fft_block(size_t fft_size)
    : liquid_fft_block()
{
    if (fft_size > 0) {
        configure(fft_size);
    }
}

liquid_fft_block::~liquid_fft_block()
{
    destroy_plan();
}

size_t liquid_fft_block::size() const noexcept
{
    return d_fft_size;
}

bool liquid_fft_block::is_configured() const noexcept
{
    return d_plan != nullptr;
}

void liquid_fft_block::destroy_plan()
{
    if (d_plan != nullptr) {
        fft_destroy_plan(d_plan);
        d_plan = nullptr;
    }

    d_fft_size = 0;
    d_time_domain.clear();
    d_frequency_domain.clear();
}

void liquid_fft_block::configure(size_t fft_size)
{
    if (fft_size == 0) {
        throw std::invalid_argument("FFT size must be greater than zero");
    }

    if (fft_size == d_fft_size && d_plan != nullptr) {
        return;
    }

    destroy_plan();

    d_fft_size = fft_size;
    d_time_domain.assign(fft_size, liquid_float_complex(0.0F, 0.0F));
    d_frequency_domain.assign(fft_size, liquid_float_complex(0.0F, 0.0F));

    d_plan = fft_create_plan(static_cast<unsigned int>(fft_size),
                             d_time_domain.data(),
                             d_frequency_domain.data(),
                             LIQUID_FFT_FORWARD,
                             0);

    if (d_plan == nullptr) {
        throw std::runtime_error("Failed to create liquid FFT plan");
    }
}

complex_vector liquid_fft_block::process(const complex_vector& input)
{
    if (input.empty()) {
        return {};
    }

    if (input.size() != d_fft_size || d_plan == nullptr) {
        configure(input.size());
    }

    std::transform(input.begin(), input.end(), d_time_domain.begin(), [](const complex_type& sample) {
        return liquid_float_complex(sample.real(), sample.imag());
    });

    fft_execute(d_plan);

    return complex_vector(d_frequency_domain.begin(), d_frequency_domain.end());
}

std::vector<float> magnitude_block::process(const complex_vector& input) const
{
    std::vector<float> magnitudes(input.size(), 0.0F);

    std::transform(input.begin(), input.end(), magnitudes.begin(), [](const complex_type& value) {
        return std::abs(value);
    });

    return magnitudes;
}

peak_detector_block::peak_detector_block(float threshold, bool relative)
    : d_threshold(threshold), d_relative(relative)
{
}

float peak_detector_block::threshold() const noexcept
{
    return d_threshold;
}

bool peak_detector_block::relative() const noexcept
{
    return d_relative;
}

void peak_detector_block::set_threshold(float threshold) noexcept
{
    d_threshold = threshold;
}

void peak_detector_block::set_relative(bool relative) noexcept
{
    d_relative = relative;
}

void peak_detector_block::configure(float threshold, bool relative) noexcept
{
    d_threshold = threshold;
    d_relative = relative;
}

std::optional<peak_detector_block::peak_info>
peak_detector_block::process(const std::vector<float>& magnitudes) const
{
    if (magnitudes.empty()) {
        return std::nullopt;
    }

    auto it = std::max_element(magnitudes.begin(), magnitudes.end());
    const float max_value = *it;

    peak_info info;
    info.index = static_cast<size_t>(std::distance(magnitudes.begin(), it));
    info.value = max_value;

    float threshold_value = d_threshold;
    if (d_relative) {
        threshold_value *= max_value;
    }

    if (threshold_value > 0.0F && max_value < threshold_value) {
        return std::nullopt;
    }

    return info;
}

standalone_rx_chain::standalone_rx_chain(const config& cfg)
    : d_cfg(cfg),
      d_source(),
      d_window(cfg.fft_size, cfg.window),
      d_fft(cfg.fft_size),
      d_magnitude(),
      d_detector(cfg.peak_threshold, cfg.relative_threshold)
{
    if (cfg.fft_size == 0) {
        throw std::invalid_argument("FFT size must be greater than zero");
    }
}

void standalone_rx_chain::set_config(const config& cfg)
{
    if (cfg.fft_size == 0) {
        throw std::invalid_argument("FFT size must be greater than zero");
    }

    d_cfg = cfg;
    d_window.set_type(cfg.window);
    d_window.set_size(cfg.fft_size);
    d_fft.configure(cfg.fft_size);
    d_detector.configure(cfg.peak_threshold, cfg.relative_threshold);
}

const standalone_rx_chain::config& standalone_rx_chain::get_config() const noexcept
{
    return d_cfg;
}

window_block& standalone_rx_chain::window() noexcept
{
    return d_window;
}

const window_block& standalone_rx_chain::window() const noexcept
{
    return d_window;
}

peak_detector_block& standalone_rx_chain::detector() noexcept
{
    return d_detector;
}

const peak_detector_block& standalone_rx_chain::detector() const noexcept
{
    return d_detector;
}

rx_result standalone_rx_chain::process(const complex_vector& input)
{
    if (d_cfg.fft_size == 0) {
        throw std::runtime_error("standalone_rx_chain is not configured");
    }

    complex_vector prepared;
    prepared.reserve(d_cfg.fft_size);

    if (input.size() < d_cfg.fft_size) {
        prepared = input;
        prepared.resize(d_cfg.fft_size, complex_type(0.0F, 0.0F));
    } else if (input.size() > d_cfg.fft_size) {
        prepared.assign(input.begin(),
                        std::next(input.begin(), static_cast<std::ptrdiff_t>(d_cfg.fft_size)));
    } else {
        prepared = input;
    }

    d_source.set_samples(prepared);

    if (d_window.size() != prepared.size()) {
        d_window.set_size(prepared.size());
    }

    if (d_window.type() != d_cfg.window) {
        d_window.set_type(d_cfg.window);
    }

    auto windowed = d_window.process(d_source.samples());

    if (!d_fft.is_configured() || d_fft.size() != windowed.size()) {
        d_fft.configure(windowed.size());
    }

    auto spectrum = d_fft.process(windowed);
    auto magnitude = d_magnitude.process(spectrum);

    if (d_cfg.normalize_magnitude && !magnitude.empty()) {
        auto max_it = std::max_element(magnitude.begin(), magnitude.end());
        if (max_it != magnitude.end() && *max_it > 0.0F) {
            const float inv_max = 1.0F / *max_it;
            for (auto& value : magnitude) {
                value *= inv_max;
            }
        }
    }

    if (d_detector.threshold() != d_cfg.peak_threshold || d_detector.relative() != d_cfg.relative_threshold) {
        d_detector.configure(d_cfg.peak_threshold, d_cfg.relative_threshold);
    }

    auto peak = d_detector.process(magnitude);

    return rx_result{std::move(prepared),
                     std::move(windowed),
                     std::move(spectrum),
                     std::move(magnitude),
                     std::move(peak)};
}

} // namespace experimental
} // namespace lora_sdr
} // namespace gr
