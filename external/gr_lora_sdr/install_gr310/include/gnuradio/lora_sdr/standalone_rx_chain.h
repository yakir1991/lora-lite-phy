/* -*- c++ -*- */
/*
 * Standalone LoRa SDR receive chain for experimental use.
 * This file is part of gr-lora_sdr and distributed under the terms
 * of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any
 * later version.
 */

#ifndef INCLUDED_LORA_SDR_STANDALONE_RX_CHAIN_H
#define INCLUDED_LORA_SDR_STANDALONE_RX_CHAIN_H

#include <gnuradio/lora_sdr/api.h>

#include <cstddef>
#include <complex>
#include <optional>
#include <vector>

#include <liquid/liquid.h>

namespace gr {
namespace lora_sdr {
namespace experimental {

using complex_type = std::complex<float>;
using complex_vector = std::vector<complex_type>;

/*!\brief Simple container that mimics a GNU Radio vector source block. */
class LORA_SDR_API vector_source
{
public:
    vector_source();
    explicit vector_source(complex_vector samples);

    void set_samples(const complex_vector& samples);
    void set_samples(complex_vector&& samples) noexcept;

    const complex_vector& samples() const noexcept;
    bool empty() const noexcept;

private:
    complex_vector d_samples;
};

/*!\brief Applies a configurable window to complex samples. */
class LORA_SDR_API window_block
{
public:
    enum class window_type { rectangular, hann, hamming, blackmanharris };

    window_block();
    window_block(size_t size, window_type type = window_type::hann);

    size_t size() const noexcept;
    window_type type() const noexcept;

    void set_size(size_t size);
    void set_type(window_type type);

    const std::vector<float>& coefficients() const noexcept;

    complex_vector process(const complex_vector& input) const;

private:
    void compute_coefficients();

    size_t d_size;
    window_type d_type;
    std::vector<float> d_coeffs;
};

/*!\brief Thin wrapper around liquid-dsp FFT plans. */
class LORA_SDR_API liquid_fft_block
{
public:
    liquid_fft_block();
    explicit liquid_fft_block(size_t fft_size);
    ~liquid_fft_block();

    size_t size() const noexcept;
    bool is_configured() const noexcept;

    void configure(size_t fft_size);

    complex_vector process(const complex_vector& input);

private:
    void destroy_plan();

    size_t d_fft_size;
    fftplan d_plan;
    std::vector<liquid_float_complex> d_time_domain;
    std::vector<liquid_float_complex> d_frequency_domain;
};

/*!\brief Computes the magnitude of complex FFT bins. */
class LORA_SDR_API magnitude_block
{
public:
    std::vector<float> process(const complex_vector& input) const;
};

/*!\brief Finds the dominant spectral peak above a configurable threshold. */
class LORA_SDR_API peak_detector_block
{
public:
    struct peak_info {
        size_t index = 0U;
        float value = 0.0F;
    };

    peak_detector_block(float threshold = 0.0F, bool relative = false);

    float threshold() const noexcept;
    bool relative() const noexcept;

    void set_threshold(float threshold) noexcept;
    void set_relative(bool relative) noexcept;
    void configure(float threshold, bool relative) noexcept;

    std::optional<peak_info> process(const std::vector<float>& magnitudes) const;

private:
    float d_threshold;
    bool d_relative;
};

/*!\brief Output bundle for the standalone receive chain. */
struct LORA_SDR_API rx_result
{
    complex_vector input;
    complex_vector windowed;
    complex_vector spectrum;
    std::vector<float> magnitude;
    std::optional<peak_detector_block::peak_info> peak;
};

/*!\brief High level, self-contained receive pipeline built from the blocks above. */
class LORA_SDR_API standalone_rx_chain
{
public:
    struct config {
        size_t fft_size = 0U;
        window_block::window_type window = window_block::window_type::hann;
        float peak_threshold = 0.0F;
        bool relative_threshold = false;
        bool normalize_magnitude = true;
    };

    explicit standalone_rx_chain(const config& cfg);

    void set_config(const config& cfg);
    const config& get_config() const noexcept;

    rx_result process(const complex_vector& input);

    window_block& window() noexcept;
    const window_block& window() const noexcept;

    peak_detector_block& detector() noexcept;
    const peak_detector_block& detector() const noexcept;

private:
    config d_cfg;
    vector_source d_source;
    window_block d_window;
    liquid_fft_block d_fft;
    magnitude_block d_magnitude;
    peak_detector_block d_detector;
};

} // namespace experimental
} // namespace lora_sdr
} // namespace gr

#endif /* INCLUDED_LORA_SDR_STANDALONE_RX_CHAIN_H */
