#pragma once

#include <complex>
#include <vector>

extern "C" {
#include "kiss_fft.h"
}

namespace host_sim
{

class FftDemodReference
{
public:
    FftDemodReference(int sf, int sample_rate, int bandwidth);
    ~FftDemodReference();

    FftDemodReference(const FftDemodReference&) = delete;
    FftDemodReference& operator=(const FftDemodReference&) = delete;

    uint16_t demodulate(const std::complex<float>* symbol_samples) const;

    int samples_per_symbol() const { return samples_per_symbol_; }
    void set_frequency_offsets(float fractional, float sfo_slope, int cfo_int);

private:
    int sf_;
    int n_bins_;
    int sample_rate_;
    int bandwidth_;
    int oversample_factor_;
    int samples_per_symbol_;

    std::vector<std::complex<float>> downchirp_;
    kiss_fft_cfg kiss_cfg_;
    mutable std::vector<kiss_fft_cpx> fft_input_;
    mutable std::vector<kiss_fft_cpx> fft_output_;
    float fractional_offset_{0.0f};
    float sfo_slope_{0.0f};
    int cfo_int_{0};

    void initialize_fft();
};

} // namespace host_sim
