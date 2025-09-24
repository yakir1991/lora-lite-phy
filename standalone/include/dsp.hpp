#pragma once
#include <complex>
#include <cstdint>
#include <span>
#include <vector>

namespace lora::standalone {

struct ChirpRefs {
    std::vector<std::complex<float>> up;
    std::vector<std::complex<float>> down;
};

ChirpRefs build_ref_chirps(uint32_t sf, uint32_t os);

// Decimate by OS with phase selection [0..os-1]
std::vector<std::complex<float>> decimate_os_phase(std::span<const std::complex<float>> x, int os, int phase);

// Dechirp block (multiply by conj(downchirp)) and peak FFT bin using naive DFT at integer bins
uint32_t demod_symbol_peak(std::span<const std::complex<float>> block,
                           std::span<const std::complex<float>> downchirp);

// CFO-aware variant: eps is fractional CFO in integer-bin units per symbol ([-0.5,0.5))
uint32_t demod_symbol_peak_cfo(std::span<const std::complex<float>> block,
                               std::span<const std::complex<float>> downchirp,
                               float eps);

// FFT-based demod: dechirp (with optional CFO) then FFT and pick max-magnitude bin
struct FFTBinPeak { uint32_t bin; float power; };
FFTBinPeak demod_symbol_peak_fft_with_power(std::span<const std::complex<float>> block,
                                            std::span<const std::complex<float>> downchirp,
                                            float eps = 0.0f);
uint32_t demod_symbol_peak_fft(std::span<const std::complex<float>> block,
                               std::span<const std::complex<float>> downchirp,
                               float eps = 0.0f);

// Try small timing offsets around the block boundary and return best bin
struct DemodResult { uint32_t bin; float power; int shift; };
DemodResult demod_symbol_peak_fft_best_shift(std::span<const std::complex<float>> block,
                                             std::span<const std::complex<float>> downchirp,
                                             float eps,
                                             int max_shift);

// Sliding correlation-like preamble detector: dechirp and check bin-0 energy for preamble_syms
struct PreambleDetectResult {
    bool found{false};
    size_t start_raw{0};
    int os{1};
    int phase{0};
};

PreambleDetectResult detect_preamble_os(std::span<const std::complex<float>> samples,
                                        uint32_t sf,
                                        size_t min_syms,
                                        std::span<const int> os_candidates);

// Estimate fractional CFO (eps) from preamble: average inter-symbol phase of dechirped bin-0 correlation
// Returns eps in bins per symbol (i.e., fraction of FFT bin), or 0 if failed
float estimate_cfo_epsilon(std::span<const std::complex<float>> decim,
                           uint32_t sf,
                           size_t start_decim,
                           size_t preamble_syms);

// Estimate integer CFO in FFT bin units from preamble (after decimation), using dechirp+FFT peak bins
int estimate_cfo_integer(std::span<const std::complex<float>> decim,
                         uint32_t sf,
                         size_t start_decim,
                         size_t preamble_syms);

// Compute a simple per-symbol "score": peak power after dechirp with down (up_score) and with up (down_score)
struct SymbolTypeScore {
    float up_score{0.f};   // large when symbol was upchirp (i.e., preamble)
    float down_score{0.f}; // large when symbol was downchirp (i.e., SFD)
};

SymbolTypeScore classify_symbol(std::span<const std::complex<float>> block,
                                std::span<const std::complex<float>> upchirp,
                                std::span<const std::complex<float>> downchirp,
                                float eps = 0.f);

// Hamming(7,4) decode: input 7-bit codeword in LSB-first (bit0=c1) or MSB-first? We'll use MSB-first vector helpers.
// Helper to decode a 7-bit array (b[0]..b[6] MSB-first) into a 4-bit value; returns corrected nibble and sets err_corrected if one-bit corrected.
uint8_t hamming74_decode_bits_msb(const uint8_t b[7], bool* err_corrected = nullptr, bool* uncorrectable = nullptr);

// Hamming(8,4) decoder for CR4/8: input 8 bits MSB-first; returns 4-bit nibble (MSB-first).
// Uses minimum Hamming distance to the 16 valid codewords per GR LoRa mapping.
uint8_t hamming84_decode_bits_msb(const uint8_t b[8], bool* err_corrected = nullptr, int* distance = nullptr);

// Hamming payload decoder helpers (min-distance over LoRa LUT)
// Returns nibble (4 bits), sets distance to Hamming distance to closest codeword.
// cw_len is 4+cr (cr in {1,2,3,4}); bits are msb-first across cw_len positions in bits8[0..cw_len-1].
uint8_t hamming_payload_decode_bits_msb(const uint8_t bits8[8], int cw_len, bool* corrected, int* distance);

} // namespace lora::standalone
