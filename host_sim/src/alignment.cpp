#include "host_sim/alignment.hpp"

#include "host_sim/fft_demod.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

extern "C" {
#include "kiss_fft.h"
}

namespace host_sim
{

std::size_t detect_burst_start(const std::vector<std::complex<float>>& samples,
                               int samples_per_symbol,
                               float threshold_factor)
{
    if (samples.empty() || samples_per_symbol <= 0) {
        return 0;
    }

    const std::size_t window = static_cast<std::size_t>(samples_per_symbol);
    const std::size_t n_windows = samples.size() / window;
    if (n_windows < 3) {
        return 0;  // Too short to distinguish noise from signal.
    }

    // Compute per-window mean power.
    std::vector<float> powers(n_windows, 0.0f);
    for (std::size_t w = 0; w < n_windows; ++w) {
        double acc = 0.0;
        for (std::size_t i = 0; i < window; ++i) {
            const auto& s = samples[w * window + i];
            acc += static_cast<double>(s.real()) * s.real() +
                   static_cast<double>(s.imag()) * s.imag();
        }
        powers[w] = static_cast<float>(acc / static_cast<double>(window));
    }

    // Estimate noise floor from lowest quartile of windows.
    std::vector<float> sorted_powers(powers);
    std::sort(sorted_powers.begin(), sorted_powers.end());
    const std::size_t q1_end = std::max<std::size_t>(1, n_windows / 4);
    double noise_acc = 0.0;
    for (std::size_t i = 0; i < q1_end; ++i) {
        noise_acc += sorted_powers[i];
    }
    const float noise_floor = static_cast<float>(noise_acc / static_cast<double>(q1_end));
    const float threshold = noise_floor * threshold_factor;

    // Find first window that exceeds threshold.
    for (std::size_t w = 0; w < n_windows; ++w) {
        if (powers[w] > threshold) {
            // Back up slightly so alignment search can scan the start.
            const std::size_t margin = (w >= 2) ? 2 : w;
            return (w - margin) * window;
        }
    }

    return 0;  // No burst detected — start from beginning.
}

AlignmentResult find_symbol_alignment_scored(
    const std::vector<std::complex<float>>& samples,
    const FftDemodulator& demod,
    int preamble_symbols)
{
    const int sps = demod.samples_per_symbol();
    if (samples.size() < static_cast<std::size_t>(sps * preamble_symbols)) {
        return {0U, 0};
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

    return {best_offset, best_score};
}

std::size_t find_symbol_alignment(const std::vector<std::complex<float>>& samples,
                                  const FftDemodulator& demod,
                                  int preamble_symbols)
{
    return find_symbol_alignment_scored(samples, demod, preamble_symbols).offset;
}

std::optional<std::size_t> find_header_symbol_index(
    const std::vector<uint16_t>& symbols,
    int sync_word,
    int sf)
{
    // LoRa sync word is 8 bits, split into two 4-bit nibbles
    // GNU Radio uses fixed shift of 3 bits regardless of SF:
    //   sync_high = ((sync_word >> 4) & 0xF) << 3
    //   sync_low = (sync_word & 0xF) << 3
    // For sync_word=0x12: sync_high=8, sync_low=16
    // This is non-standard but we match GNU Radio for compatibility.
    const int sync_high = ((sync_word >> 4) & 0xF) << 3;
    const int sync_low = (sync_word & 0xF) << 3;
    
    const int N = 1 << sf;
    const int tolerance = 2;  // Allow small deviation due to noise

    // Search for sync words pattern: should find sync_high followed by sync_low
    for (std::size_t i = 0; i + 1 < symbols.size(); ++i) {
        int sym_high = static_cast<int>(symbols[i]);
        int sym_low = static_cast<int>(symbols[i + 1]);
        
        // Check if this position matches sync words (with tolerance)
        int diff_high = std::abs(sym_high - sync_high);
        int diff_low = std::abs(sym_low - sync_low);
        
        // Handle wrap-around for circular FFT values
        diff_high = std::min(diff_high, N - diff_high);
        diff_low = std::min(diff_low, N - diff_low);
        
        if (diff_high <= tolerance && diff_low <= tolerance) {
            // Found sync words at position i, i+1
            // After sync words, there are 2.25 downchirp symbols (SFD)
            // Header starts at: sync_position + 2 (sync words) + 2.25 (SFD)
            // But since we're using integer symbol indices and the 0.25 is 
            // handled by sample-level alignment, we approximate:
            // Header = sync + 2 + 2 = sync + 4 (but actual is sync + 4.25)
            // 
            // Actually, looking at the analysis:
            // - Preamble: symbols 0-6 (7 symbols)
            // - Sync words: symbols 7-8 (sync_high, sync_low)
            // - SFD: symbols 9-10 + quarter
            // - Header: starts at symbol 11.25
            //
            // So header_start = sync_word_high_index + 4.25
            // But since we demodulate at full symbols, we need to account for
            // the quarter symbol offset in sample-level alignment, not here.
            //
            // For now, return sync_word_high_index + 4 and note that
            // the sample alignment may need a quarter-symbol shift.
            
            return i + 4;  // Approximate: might need +4.25 for full accuracy
        }
    }
    
    return std::nullopt;
}

PreambleSearchResult find_symbol_alignment_cfo_aware(
    const std::vector<std::complex<float>>& samples,
    const FftDemodulator& demod,
    int preamble_symbols)
{
    const int sps = demod.samples_per_symbol();
    const int n_bins = 1 << demod.sf();
    const int os = demod.oversample_factor();

    PreambleSearchResult result{};

    if (samples.size() < static_cast<std::size_t>(sps * preamble_symbols)) {
        return result;
    }

    // --- Phase 1: coarse scan with stride to find candidate preamble bin ---
    // Scanning all offsets × all symbols is O(sps × preamble_symbols × N).
    // To keep it fast, first scan with a stride to narrow the offset range.
    // The polyphase fold sinc main lobe is ~os samples wide, so stride=os
    // won't miss the peak, and the fine scan refines within ±os.
    const int coarse_stride = std::max(1, os);  // full oversampling factor
    const int coarse_steps = (sps + coarse_stride - 1) / coarse_stride;

    // For each coarse offset, dechirp preamble_symbols symbols and compare
    // accumulated peak magnitudes.  The true alignment yields the strongest
    // polyphase-fold response because the anti-aliasing filter is matched.
    int global_best_bin = 0;
    int global_best_offset_coarse = 0;

    // Allocate FFT resources once
    kiss_fft_cfg cfg = kiss_fft_alloc(n_bins, 0, nullptr, nullptr);
    if (!cfg) {
        // Fallback to legacy alignment
        result.alignment_offset = find_symbol_alignment(samples, demod, preamble_symbols);
        return result;
    }

    const auto& downchirp = demod.chirps().downchirp;
    std::vector<kiss_fft_cpx> fft_in(n_bins);
    std::vector<kiss_fft_cpx> fft_out(n_bins);

    auto dechirp_symbol = [&](std::size_t sample_offset, float* out_peak_mag = nullptr) -> int {
        // Polyphase fold + FFT to find peak bin (same as FftDemodulator::compute_fft)
        for (int bin = 0; bin < n_bins; ++bin) {
            std::complex<float> acc{0.0f, 0.0f};
            for (int m = 0; m < os; ++m) {
                const int idx = bin * os + m;
                acc += samples[sample_offset + idx] * downchirp[idx];
            }
            fft_in[bin].r = acc.real();
            fft_in[bin].i = acc.imag();
        }
        kiss_fft(cfg, fft_in.data(), fft_out.data());

        int best_bin = 0;
        float best_mag = -1.0f;
        for (int bin = 0; bin < n_bins; ++bin) {
            const float mag = fft_out[bin].r * fft_out[bin].r +
                              fft_out[bin].i * fft_out[bin].i;
            if (mag > best_mag) {
                best_mag = mag;
                best_bin = bin;
            }
        }
        if (out_peak_mag) {
            *out_peak_mag = best_mag;
        }
        return best_bin;
    };

    // Coarse scan — score by accumulated peak MAGNITUDE of the majority bin.
    // At the true alignment the polyphase fold response is maximal;
    // at aliased offsets the fold's anti-aliasing filter attenuates it.
    // Cache per-symbol results to avoid redundant dechirps.
    std::vector<int> sym_peaks(preamble_symbols);
    float global_best_mag_sum = -1.0f;
    for (int ci = 0; ci < coarse_steps; ++ci) {
        const int offset = ci * coarse_stride;
        std::vector<float> mag_per_bin(n_bins, 0.0f);

        int valid_syms = 0;
        for (int sym = 0; sym < preamble_symbols; ++sym) {
            std::size_t base_sample = static_cast<std::size_t>(offset) +
                                      static_cast<std::size_t>(sym) * sps;
            if (base_sample + sps > samples.size()) break;
            float peak_mag = 0.0f;
            int peak = dechirp_symbol(base_sample, &peak_mag);
            sym_peaks[sym] = peak;
            mag_per_bin[peak] += peak_mag;
            // Also count near-neighbors (±1) as the same bin
            int left = (peak - 1 + n_bins) % n_bins;
            int right = (peak + 1) % n_bins;
            mag_per_bin[left] += peak_mag * 0.01f; // tiny boost for neighbor
            mag_per_bin[right] += peak_mag * 0.01f;
            ++valid_syms;
        }

        // Find the bin with the highest accumulated magnitude.
        int local_best_bin = 0;
        float local_best_mag = -1.0f;
        for (int b = 0; b < n_bins; ++b) {
            if (mag_per_bin[b] > local_best_mag) {
                local_best_mag = mag_per_bin[b];
                local_best_bin = b;
            }
        }

        // Require at least half of preamble symbols in the dominant bin ±1.
        // Use cached peaks — no redundant dechirp.
        int count_near = 0;
        for (int sym = 0; sym < valid_syms; ++sym) {
            int diff = std::abs(sym_peaks[sym] - local_best_bin);
            diff = std::min(diff, n_bins - diff);
            if (diff <= 1) ++count_near;
        }

        if (count_near >= (preamble_symbols + 1) / 2 &&
            local_best_mag > global_best_mag_sum) {
            global_best_mag_sum = local_best_mag;
            global_best_bin = local_best_bin;
            global_best_offset_coarse = offset;
        }
    }

    // --- Phase 2: fine scan around coarse winner ---
    int fine_start = std::max(0, global_best_offset_coarse - coarse_stride);
    int fine_end = std::min(sps - 1, global_best_offset_coarse + coarse_stride);

    float best_mag_sum = -1.0f;
    std::size_t best_offset = static_cast<std::size_t>(global_best_offset_coarse);
    int best_bin = global_best_bin;

    for (int offset = fine_start; offset <= fine_end; ++offset) {
        std::vector<float> fine_mag_per_bin(n_bins, 0.0f);

        for (int sym = 0; sym < preamble_symbols; ++sym) {
            std::size_t base_sample = static_cast<std::size_t>(offset) +
                                      static_cast<std::size_t>(sym) * sps;
            if (base_sample + sps > samples.size()) break;
            float peak_mag = 0.0f;
            int peak = dechirp_symbol(base_sample, &peak_mag);
            fine_mag_per_bin[peak] += peak_mag;
        }

        // Find dominant bin by accumulated magnitude
        int dominant_bin = 0;
        float dominant_mag = -1.0f;
        for (int b = 0; b < n_bins; ++b) {
            if (fine_mag_per_bin[b] > dominant_mag) {
                dominant_mag = fine_mag_per_bin[b];
                dominant_bin = b;
            }
        }

        if (dominant_mag > best_mag_sum * 1.001f) {
            // Require >0.1% improvement to switch offset — avoids bouncing
            // between nearby offsets that differ only by noise.
            best_mag_sum = dominant_mag;
            best_offset = static_cast<std::size_t>(offset);
            best_bin = dominant_bin;
        }
    }

    free(cfg);

    result.alignment_offset = best_offset;
    result.preamble_bin = best_bin;
    result.score = static_cast<int>(best_mag_sum > 0.0f ? preamble_symbols : 0);

    return result;
}

} // namespace host_sim
