#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

#include "lora/workspace.hpp"

namespace lora::rx::gr {

struct PreambleDetectResult {
    size_t start_sample{};
    int os{1};
    int phase{0};
};

std::vector<std::complex<float>> decimate_os_phase(std::span<const std::complex<float>> x,
                                                   int os,
                                                   int phase,
                                                   float attenuation_db = 80.0f);

std::optional<size_t> detect_preamble(Workspace& ws,
                                      std::span<const std::complex<float>> samples,
                                      uint32_t sf,
                                      size_t min_syms = 8u);

std::optional<float> estimate_cfo_from_preamble(Workspace& ws,
                                                std::span<const std::complex<float>> samples,
                                                uint32_t sf,
                                                size_t start_sample,
                                                size_t preamble_syms);

std::optional<int> estimate_sto_from_preamble(Workspace& ws,
                                              std::span<const std::complex<float>> samples,
                                              uint32_t sf,
                                              size_t start_sample,
                                              size_t preamble_syms,
                                              int search_radius);

std::optional<PreambleDetectResult> detect_preamble_os(Workspace& ws,
                                                       std::span<const std::complex<float>> samples,
                                                       uint32_t sf,
                                                       size_t min_syms,
                                                       const std::vector<int>& os_candidates);

std::optional<PreambleDetectResult> detect_preamble_os(Workspace& ws,
                                                       std::span<const std::complex<float>> samples,
                                                       uint32_t sf,
                                                       size_t min_syms,
                                                       std::initializer_list<int> os_candidates);

uint32_t demod_symbol_peak(Workspace& ws, const std::complex<float>* block);

} // namespace lora::rx::gr
