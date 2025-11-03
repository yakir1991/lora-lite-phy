#include "host_sim/fft_demod_q15.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace host_sim
{

FftDemodulatorQ15::FftDemodulatorQ15(int sf, int sample_rate, int bandwidth)
    : sf_(sf),
      n_bins_(1 << sf),
      sample_rate_(sample_rate),
      bandwidth_(bandwidth),
      oversample_factor_(std::max(1, sample_rate / bandwidth)),
      samples_per_symbol_((1 << sf) * oversample_factor_),
      float_demod_(sf, sample_rate, bandwidth)
{
    scratch_.resize(samples_per_symbol_);
}

void FftDemodulatorQ15::set_input_scale(float scale)
{
    input_scale_ = (scale > 0.0f) ? scale : 1.0f;
}

uint16_t FftDemodulatorQ15::demodulate(const Q15Complex* symbol_samples)
{
    const float inv_scale = (input_scale_ == 0.0f) ? 1.0f : (1.0f / input_scale_);
    for (int i = 0; i < samples_per_symbol_; ++i) {
        scratch_[i] = {
            q15_to_float(symbol_samples[i].real) * inv_scale,
            q15_to_float(symbol_samples[i].imag) * inv_scale,
        };
    }
    ++symbol_counter_;
    return float_demod_.demodulate(scratch_.data());
}

void FftDemodulatorQ15::set_frequency_offsets(float cfo_frac, int cfo_int, float sfo_slope)
{
    cfo_frac_ = cfo_frac;
    cfo_int_ = cfo_int;
    sfo_slope_ = sfo_slope;
    float_demod_.set_frequency_offsets(cfo_frac, cfo_int, sfo_slope);
}

} // namespace host_sim
