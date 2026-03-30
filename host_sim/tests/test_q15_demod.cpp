/// test_q15_demod.cpp — Verify that the native Q15 demodulator produces
/// the same symbol outputs as the float reference for clean chirps across
/// all supported SF values (SF6-SF12).

#include "host_sim/chirp.hpp"
#include "host_sim/fft_demod.hpp"
#include "host_sim/fft_demod_q15.hpp"
#include "host_sim/q15.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main()
{
    int total = 0;
    int mismatches = 0;

    for (int sf = 6; sf <= 12; ++sf) {
        const int n_bins = 1 << sf;
        const int bw = 125000;
        const int sr = bw; // OS=1

        host_sim::FftDemodulator   float_demod(sf, sr, bw);
        host_sim::FftDemodulatorQ15 q15_demod(sf, sr, bw);

        const auto chirps_f = host_sim::build_chirps(sf, 1);
        const auto chirps_q = host_sim::build_chirps_q15(sf, 1);

        // Test a spread of symbol values.
        const int step = (n_bins > 64) ? (n_bins / 32) : 1;
        for (int sym = 0; sym < n_bins; sym += step) {
            // Build a clean modulated symbol: upchirp shifted by `sym`.
            std::vector<std::complex<float>> samples_f(n_bins);
            std::vector<host_sim::Q15Complex> samples_q(n_bins);
            for (int i = 0; i < n_bins; ++i) {
                const int idx = (i + sym) % n_bins;
                samples_f[i] = chirps_f.upchirp[idx];
                samples_q[i] = chirps_q.upchirp[idx];
            }

            float_demod.reset_symbol_counter();
            q15_demod.reset_symbol_counter();

            const uint16_t ref = float_demod.demodulate(samples_f.data());
            const uint16_t q15 = q15_demod.demodulate(samples_q.data());

            ++total;
            if (ref != q15) {
                std::fprintf(stderr,
                    "MISMATCH SF%d sym=%d: float=%u q15=%u\n",
                    sf, sym, ref, q15);
                ++mismatches;
            }
        }
    }

    std::printf("Q15 demod test: %d/%d match", total - mismatches, total);
    if (mismatches > 0) {
        std::printf(" (%d mismatches)\n", mismatches);
        return 1;
    }
    std::printf("\n");
    return 0;
}
