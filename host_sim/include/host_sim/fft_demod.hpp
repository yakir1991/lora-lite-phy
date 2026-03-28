#pragma once

#include "host_sim/chirp.hpp"

#include <complex>
#include <cstddef>
#include <vector>

extern "C" {
#include "kiss_fft.h"
}

namespace host_sim
{

class FftDemodulator
{
public:
    FftDemodulator(int sf, int sample_rate, int bandwidth);
    ~FftDemodulator();

    FftDemodulator(const FftDemodulator&) = delete;
    FftDemodulator& operator=(const FftDemodulator&) = delete;

    uint16_t demodulate(const std::complex<float>* symbol_samples) const;

    // Return |fft_out_[n]|² for all N bins after the most recent demodulate() call.
    std::vector<float> get_fft_magnitudes_sq() const;

    int samples_per_symbol() const { return samples_per_symbol_; }

    int oversample_factor() const { return oversample_factor_; }

    int sf() const { return sf_; }

    struct FrequencyEstimate
    {
        float cfo_frac{0.0f};
        int cfo_int{0};
        float sfo_slope{0.0f};
    };

    FrequencyEstimate estimate_frequency_offsets(const std::complex<float>* samples,
                                                 int symbol_count) const;
    void set_frequency_offsets(float cfo_frac, int cfo_int, float sfo_slope);
    void reset_symbol_counter() const { symbol_counter_ = 0; }

    float current_cfo_frac() const { return cfo_frac_; }
    int current_cfo_int() const { return cfo_int_; }
    float current_sfo_slope() const { return sfo_slope_; }

    const ChirpTables& chirps() const { return chirps_; }

private:
    int sf_;
    int n_bins_;
    int sample_rate_;
    int bandwidth_;
    int oversample_factor_;
    int samples_per_symbol_;
    ChirpTables chirps_;
    kiss_fft_cfg kiss_cfg_{nullptr};
    mutable std::vector<kiss_fft_cpx> fft_in_;
    mutable std::vector<kiss_fft_cpx> fft_out_;
    float cfo_frac_{0.0f};
    int cfo_int_{0};
    float sfo_slope_{0.0f};
    mutable std::size_t symbol_counter_{0};

    void initialize_fft();
    void compute_fft(const std::complex<float>* symbol_samples,
                     kiss_fft_cpx* output) const;
};

}
