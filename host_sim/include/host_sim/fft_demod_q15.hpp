#pragma once

#include "host_sim/chirp.hpp"
#include "host_sim/q15.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// Forward-declare the opaque KissFFT Q15 config.
struct kiss_fft_state;
typedef struct kiss_fft_state* kiss_fft_q15_cfg;

namespace host_sim
{

/// Native Q15 fixed-point LoRa demodulator.
///
/// All signal processing (downchirp multiply, FFT, peak detection) runs
/// in Q15/Q31 arithmetic.  Only the final parabolic interpolation and
/// CFO-tracking EMA use a few floats.
class FftDemodulatorQ15
{
public:
    FftDemodulatorQ15(int sf, int sample_rate, int bandwidth);
    ~FftDemodulatorQ15();

    FftDemodulatorQ15(const FftDemodulatorQ15&) = delete;
    FftDemodulatorQ15& operator=(const FftDemodulatorQ15&) = delete;

    uint16_t demodulate(const Q15Complex* symbol_samples);

    int samples_per_symbol() const { return samples_per_symbol_; }
    int oversample_factor() const { return oversample_factor_; }
    int sf() const { return sf_; }

    void set_frequency_offsets(float cfo_frac, int cfo_int, float sfo_slope);
    void set_input_scale(float scale);
    void set_cfo_tracking(float alpha, int delay_symbols = 0);

    void reset_symbol_counter() { symbol_counter_ = 0; }

private:
    int sf_;
    int n_bins_;
    int sample_rate_;
    int bandwidth_;
    int oversample_factor_;
    int samples_per_symbol_;

    ChirpTablesQ15 chirps_q15_;
    kiss_fft_q15_cfg kiss_cfg_{nullptr};

    // Scratch buffers (pre-allocated, reused per demodulate call).
    struct Q15Cpx { int16_t r; int16_t i; };
    std::vector<Q15Cpx> fft_in_;
    std::vector<Q15Cpx> fft_out_;

    float cfo_frac_{0.0f};
    int cfo_int_{0};
    float sfo_slope_{0.0f};
    float cfo_track_alpha_{0.0f};
    int cfo_track_delay_{0};
    std::size_t symbol_counter_{0};
    float input_scale_{1.0f};
};

} // namespace host_sim
