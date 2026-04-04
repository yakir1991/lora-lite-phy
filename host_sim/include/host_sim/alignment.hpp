#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>
#include <optional>

namespace host_sim
{

class FftDemodulator;

/// Detect the sample offset where a LoRa burst begins using a sliding
/// power envelope.  Returns the sample index where signal power first
/// rises above `threshold_factor` times the estimated noise floor.
/// If no burst is found, returns std::nullopt.
/// If search_from > 0, scanning starts at that sample offset.
std::optional<std::size_t> detect_burst_start(const std::vector<std::complex<float>>& samples,
                               int samples_per_symbol,
                               float threshold_factor = 6.0f,
                               std::size_t search_from = 0);

/// Pointer+size overload — avoids vector copy in streaming paths.
std::optional<std::size_t> detect_burst_start(const std::complex<float>* samples,
                               std::size_t n_samples,
                               int samples_per_symbol,
                               float threshold_factor = 6.0f,
                               std::size_t search_from = 0);

/// Result of burst detection with signal quality info.
struct BurstDetectResult {
    std::size_t burst_start{0};     ///< Sample index of burst start
    float noise_floor{0.0f};        ///< Estimated noise power per sample
    float signal_power{0.0f};       ///< Mean power in burst-start window
};

/// Extended burst detection — returns noise floor + signal power for
/// SNR estimation.  Avoids the caller having to recompute window powers.
/// @param samples           Pointer to IQ sample buffer.
/// @param n_samples         Number of complex samples in the buffer.
/// @param samples_per_symbol Samples per LoRa symbol (FFT size × oversample).
/// @param threshold_factor  Power ratio threshold for burst detection (default 6).
/// @param search_from       Sample offset to start searching from (default 0).
/// @param prior_noise       If > 0, use as noise estimate instead of quartile.
///                          Useful for streaming: reuse noise from previous burst.
/// @param min_consec        Minimum consecutive windows above threshold to confirm
///                          a burst (default 1).  Higher values reject impulse noise.
std::optional<BurstDetectResult> detect_burst_ex(
    const std::complex<float>* samples,
    std::size_t n_samples,
    int samples_per_symbol,
    float threshold_factor = 6.0f,
    std::size_t search_from = 0,
    float prior_noise = 0.0f,
    int min_consec = 1);

std::size_t find_symbol_alignment(std::span<const std::complex<float>> samples,
                                  const FftDemodulator& demod,
                                  int preamble_symbols = 8);

/// Alignment result with confidence score.
struct AlignmentResult
{
    std::size_t offset{0};
    int score{0};    ///< Sum of per-symbol scores (2 for bin==0, 1 for bin<=2).
};

/// Same as find_symbol_alignment but also returns the best score.
AlignmentResult find_symbol_alignment_scored(
    std::span<const std::complex<float>> samples,
    const FftDemodulator& demod,
    int preamble_symbols = 8);

/// Result of CFO-aware preamble search.
struct PreambleSearchResult
{
    /// Best sample-level alignment offset within one symbol period.
    std::size_t alignment_offset{0};
    /// The FFT bin where preamble concentrates at this alignment offset.
    /// NOTE: this is the *aliased* bin (depends on alignment offset).
    /// The relationship is: preamble_bin = (true_CFO + offset/os) mod N.
    /// Use this value directly as cfo_int for consistent demodulation.
    int preamble_bin{0};
    /// Confidence score (higher = more preamble symbols matched).
    int score{0};
};

/// CFO-aware preamble search.  Works even when the carrier frequency offset
/// is large (tens of kHz).  Scans all N bins and all sample offsets to find
/// the combination that yields the longest run of identical-bin symbols.
/// The returned preamble_bin is the raw demodulated bin value, which equals
/// the integer CFO that should be fed to set_frequency_offsets().
PreambleSearchResult find_symbol_alignment_cfo_aware(
    std::span<const std::complex<float>> samples,
    const FftDemodulator& demod,
    int preamble_symbols = 8);

/// Find the symbol index where the header begins, by detecting sync words.
/// Returns std::nullopt if sync words cannot be found.
/// sync_word is the 8-bit sync word (e.g., 0x12), which is split into
/// sync_high = (sync_word >> 4) << 3 and sync_low = (sync_word & 0xF) << 3.
std::optional<std::size_t> find_header_symbol_index(
    const std::vector<uint16_t>& symbols,
    int sync_word,
    int sf);

} // namespace host_sim
