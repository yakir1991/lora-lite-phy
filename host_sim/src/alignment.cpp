#include "host_sim/alignment.hpp"

#include "host_sim/fft_demod.hpp"

#include <algorithm>

namespace host_sim
{

std::size_t find_symbol_alignment(const std::vector<std::complex<float>>& samples,
                                  const FftDemodulator& demod,
                                  int preamble_symbols)
{
    const int sps = demod.samples_per_symbol();
    if (samples.size() < static_cast<std::size_t>(sps * preamble_symbols)) {
        return 0U;
    }

    std::size_t best_offset = 0U;
    int best_score = -1;

    for (int offset = 0; offset < sps; ++offset) {
        int score = 0;
        std::size_t base = offset;
        for (int sym = 0; sym < preamble_symbols; ++sym) {
            if (base + sps > samples.size()) {
                break;
            }
            uint16_t value = demod.demodulate(&samples[base]);
            if (value == 0) {
                score += 2;
            } else if (value <= 2) {
                score += 1;
            }
            base += sps;
        }
        if (score > best_score) {
            best_score = score;
            best_offset = static_cast<std::size_t>(offset);
        }
    }

    return best_offset;
}

} // namespace host_sim
