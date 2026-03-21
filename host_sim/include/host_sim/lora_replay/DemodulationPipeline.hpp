#pragma once

#include "host_sim/fft_demod.hpp"
#include "host_sim/fft_demod_ref.hpp"
#include "host_sim/lora_params.hpp"
#include "host_sim/lora_replay/options.hpp"
#include <complex>
#include <vector>
#include <optional>
#include <cstdint>

namespace host_sim::lora_replay {

struct DemodulationResult {
    std::vector<uint16_t> symbols;
    std::vector<uint16_t> ref_symbols;
    std::vector<uint16_t> normalized_symbols;
    std::vector<host_sim::FftDemodulator::SymbolLogEntry> demod_bins;
    std::vector<uint16_t> cfo_bins;
    std::optional<host_sim::FftDemodulator::FrequencyEstimate> freq_estimate_log;
    int freq_estimate_symbols = 0;
    std::optional<uint16_t> freq_estimate_global_bin;
    std::size_t alignment_samples = 0;
    std::size_t reference_mismatches = 0;
    bool truncated_preamble_obs = false;
    int observed_preamble_symbols = 0;
};

class DemodulationPipeline {
public:
    DemodulationPipeline(const host_sim::LoRaMetadata& metadata, const Options& options);

    DemodulationResult process(const std::vector<std::complex<float>>& samples,
                               std::optional<std::size_t> known_sync_offset,
                               const std::vector<uint16_t>& reference_fft,
                               std::size_t alignment_search_start = 0);
    DemodulationResult demod_from_offset(const std::vector<std::complex<float>>& samples,
                                         std::size_t start_sample,
                                         int symbol_limit = -1);

private:
    host_sim::LoRaMetadata metadata_;
    const Options& options_;
    host_sim::FftDemodulator demod_;
    host_sim::FftDemodReference demod_ref_;
    
    std::optional<host_sim::FftDemodulator::FrequencyEstimate> cached_freq_est_;
    std::optional<float> cached_sto_frac_;

    bool run_demod_pass(const std::vector<std::complex<float>>& samples,
                        DemodulationResult& result,
                        std::size_t start_sample,
                        bool reuse_freq,
                        bool capture_bins,
                        int symbol_limit);

    int score_alignment(const std::vector<uint16_t>& normalized,
                        const std::vector<uint16_t>& reference_fft);

    struct ShiftCandidate {
        std::size_t shift{0};
        int score{0};
        std::vector<uint16_t> preview;
    };

    std::vector<ShiftCandidate> compute_symbol_candidates(const std::vector<uint16_t>& normalized,
                                                          const std::vector<uint16_t>& reference_fft,
                                                          int top_k);
};

} // namespace host_sim::lora_replay
