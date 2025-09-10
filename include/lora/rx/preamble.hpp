#pragma once
#include <cstdint>
#include <complex>
#include <optional>
#include <span>
#include <utility>
#include "lora/workspace.hpp"
#include "lora/utils/hamming.hpp"
#include "lora/constants.hpp"

namespace lora::rx {

// Detect a run of 'min_syms' upchirps (preamble) and return the starting
// sample index (0-based) if found. Assumes OS=1; performs a small sample-level
// search to tolerate integer misalignment.
std::optional<size_t> detect_preamble(Workspace& ws,
                                      std::span<const std::complex<float>> samples,
                                      uint32_t sf,
                                      size_t min_syms = 8);

// Estimate normalized CFO (cycles per sample) from a run of upchirps.
// Returns small value epsilon such that compensation is exp(-j 2π ε n).
std::optional<float> estimate_cfo_from_preamble(Workspace& ws,
                                                std::span<const std::complex<float>> samples,
                                                uint32_t sf,
                                                size_t start_sample,
                                                size_t preamble_syms);

// Estimate integer STO (sample timing offset) around the detected preamble.
// Searches shifts in [-search, +search] and returns the shift (samples) that
// maximizes correlation with the reference upchirp. Positive shift means the
// true symbol boundary is 'later' (drop samples at the front).
std::optional<int> estimate_sto_from_preamble(Workspace& ws,
                                              std::span<const std::complex<float>> samples,
                                              uint32_t sf,
                                              size_t start_sample,
                                              size_t preamble_syms,
                                              int search = 32);

// Decode payload by detecting preamble, estimating CFO over the preamble,
// compensating CFO on the entire buffer, and then running the payload decode.
std::pair<std::span<uint8_t>, bool> decode_with_preamble_cfo(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms = 8,
    uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC);

// Decode with preamble, CFO estimation and integer STO alignment.
std::pair<std::span<uint8_t>, bool> decode_with_preamble_cfo_sto(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms = 8,
    uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC);

struct PreambleDetectResult {
    size_t start_sample{}; // in original (possibly oversampled) stream
    int os{1};             // oversampling factor used to decimate
    int phase{0};          // starting phase in [0, os)
};

// Try OS candidates (e.g., {1,2,4,8}) and phases to detect preamble. On success
// returns start sample in the raw stream together with the chosen OS and phase.
std::optional<PreambleDetectResult> detect_preamble_os(Workspace& ws,
                                                       std::span<const std::complex<float>> samples,
                                                       uint32_t sf,
                                                       size_t min_syms = 8,
                                                       std::initializer_list<int> os_candidates = {1,2,4,8});

// End-to-end decode that supports OS>1 by decimating to OS=1 first, then
// applying preamble/CFO/STO logic.
std::pair<std::span<uint8_t>, bool> decode_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms = 8,
    uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC,
    std::initializer_list<int> os_candidates = {1,2,4,8});

// Full decode with preamble + sync: finds preamble (min_syms), checks the
// following sync word symbol, then decodes the rest as payload using the
// local MVP path. Returns decoded payload span and status.
std::pair<std::span<uint8_t>, bool> decode_with_preamble(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t payload_len,
    size_t min_preamble_syms = 8,
    uint8_t expected_sync = lora::LORA_SYNC_WORD_PUBLIC);

} // namespace lora::rx
