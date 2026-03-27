#pragma once

#include <complex>
#include <cstddef>
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

std::size_t find_symbol_alignment(const std::vector<std::complex<float>>& samples,
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
    const std::vector<std::complex<float>>& samples,
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
    const std::vector<std::complex<float>>& samples,
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
