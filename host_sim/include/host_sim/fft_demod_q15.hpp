#pragma once

#include "host_sim/chirp.hpp"
#include "host_sim/fft_demod.hpp"
#include "host_sim/q15.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace host_sim
{

class FftDemodulatorQ15
{
public:
    FftDemodulatorQ15(int sf, int sample_rate, int bandwidth);

    uint16_t demodulate(const Q15Complex* symbol_samples);

    int samples_per_symbol() const { return samples_per_symbol_; }
    int oversample_factor() const { return oversample_factor_; }
    int sf() const { return sf_; }

    void set_frequency_offsets(float cfo_frac, int cfo_int, float sfo_slope);
    void set_input_scale(float scale);

    void reset_symbol_counter() { symbol_counter_ = 0; }

private:
    int sf_;
    int n_bins_;
    int sample_rate_;
    int bandwidth_;
    int oversample_factor_;
    int samples_per_symbol_;
    FftDemodulator float_demod_;
    std::vector<std::complex<float>> scratch_;

    float cfo_frac_{0.0f};
    int cfo_int_{0};
    float sfo_slope_{0.0f};
    std::size_t symbol_counter_{0};
    float input_scale_{1.0f};
};

} // namespace host_sim
