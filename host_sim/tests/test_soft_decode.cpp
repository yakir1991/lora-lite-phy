#include "host_sim/deinterleaver.hpp"
#include "host_sim/hamming.hpp"
#include "host_sim/soft_decode.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <vector>

// Test: Given known symbols, verify that compute_symbol_llrs produces LLR signs
// that match the hard-decision bits from the deinterleaver.

int main()
{
    // SF=7, CR=1, payload (not header, not ldro)
    const int sf = 7;
    const int cr = 1;
    const bool is_header = false;
    const bool ldro = false;
    const bool use_ldro = ldro || is_header;
    const int sf_app = use_ldro ? sf - 2 : sf;
    const int N = 1 << sf; // 128
    const int cw_len = is_header ? 8 : cr + 4; // 5

    // Test symbols: use known values
    std::vector<uint16_t> symbols = {42, 99, 7, 120, 55};
    assert(static_cast<int>(symbols.size()) == cw_len);

    // --- Hard path ---
    host_sim::DeinterleaverConfig cfg{sf, cr, is_header, ldro};
    std::size_t consumed_hard = 0;
    auto codewords_hard = host_sim::deinterleave(symbols, cfg, consumed_hard);
    auto nibbles_hard = host_sim::hamming_decode_block(codewords_hard, is_header, cr);

    std::cout << "Hard codewords (" << codewords_hard.size() << "): ";
    for (auto c : codewords_hard) std::cout << std::setw(3) << (int)c << " ";
    std::cout << "\n";
    std::cout << "Hard nibbles:    ";
    for (auto n : nibbles_hard) std::cout << std::setw(3) << (int)n << " ";
    std::cout << "\n\n";

    // --- Simulate LLRs for clean signal ---
    // For each symbol, create an FFT magnitude array where the correct bin
    // has magnitude 1.0 and all others have 0.0.
    std::vector<host_sim::SymbolLLR> sym_llrs;
    for (int s = 0; s < cw_len; ++s) {
        std::vector<float> fft_mag(N, 0.0f);
        fft_mag[symbols[s]] = 1.0f;
        auto llr = host_sim::compute_symbol_llrs(fft_mag.data(), sf, use_ldro);
        sym_llrs.push_back(llr);

        // Print LLR signs as bits
        std::cout << "Symbol " << symbols[s] << " LLR signs: ";
        for (int b = 0; b < sf_app; ++b) {
            std::cout << (llr[b] > 0 ? "1" : "0");
        }
        std::cout << "  (LLR values:";
        for (int b = 0; b < sf_app; ++b) {
            std::cout << " " << std::setprecision(3) << llr[b];
        }
        std::cout << ")\n";
    }

    // Also print what the hard deinterleaver produces as bit matrix
    // (before deinterleave)
    std::cout << "\nHard inter_matrix (before deinterleave):\n";
    const uint16_t mask_full = (1u << sf) - 1u;
    const uint16_t mask_app = (1u << sf_app) - 1u;
    for (int i = 0; i < cw_len; ++i) {
        uint16_t raw = (symbols[i] - 1u) & mask_full;
        if (use_ldro) raw >>= 2;
        uint16_t mapped = raw ^ (raw >> 1);  // gray encode
        mapped &= mask_app;
        std::cout << "  sym=" << std::setw(3) << symbols[i]
                  << " raw=" << std::setw(3) << ((symbols[i] - 1u) & mask_full)
                  << " gray=" << std::setw(3) << mapped << " bits=";
        for (int b = 0; b < sf_app; ++b) {
            std::cout << ((mapped >> (sf_app - 1 - b)) & 1);
        }
        std::cout << "\n";
    }

    // --- Soft path ---
    std::size_t consumed_soft = 0;
    auto soft_codewords = host_sim::deinterleave_soft(
        sym_llrs, sf, cr, is_header, ldro, consumed_soft);

    std::cout << "\nSoft codeword LLR signs:\n";
    for (int r = 0; r < (int)soft_codewords.size(); ++r) {
        std::cout << "  cw[" << r << "]: ";
        for (int c = 0; c < cw_len; ++c) {
            std::cout << (soft_codewords[r][c] > 0 ? "1" : "0");
        }
        std::cout << " (hard cw=" << std::setw(3) << (int)codewords_hard[r] << " = ";
        for (int c = cw_len - 1; c >= 0; --c) {
            std::cout << ((codewords_hard[r] >> c) & 1);
        }
        std::cout << ")\n";
    }

    auto nibbles_soft = host_sim::soft_decode_block(
        sym_llrs, sf, cr, is_header, ldro, consumed_soft);

    std::cout << "\nSoft nibbles:    ";
    for (auto n : nibbles_soft) std::cout << std::setw(3) << (int)n << " ";
    std::cout << "\n";

    // Compare
    bool match = (nibbles_hard.size() == nibbles_soft.size());
    for (size_t i = 0; match && i < nibbles_hard.size(); ++i) {
        if (nibbles_hard[i] != nibbles_soft[i]) match = false;
    }
    std::cout << "\nResult: " << (match ? "MATCH" : "MISMATCH") << "\n";

    return match ? 0 : 1;
}
