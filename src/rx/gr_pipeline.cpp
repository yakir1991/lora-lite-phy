#include "lora/rx/gr_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <optional>
#include <utility>

#define LORA_PIPELINE_DEBUG 1

#include "lora/rx/gr/primitives.hpp"
#include "lora/rx/gr/header_decode.hpp"
#include "lora/rx/gr/utils.hpp"

namespace lora::rx::pipeline {

namespace {

using lora::rx::gr::PreambleDetectResult;

bool determine_ldro(const Config& cfg) {
    if (cfg.ldro_override) return *cfg.ldro_override;
    if (cfg.bandwidth_hz <= 0.0f) return cfg.sf >= 11u;
    const double n = static_cast<double>(1u << cfg.sf);
    const double symbol_duration_ms = (n * 1000.0) / static_cast<double>(cfg.bandwidth_hz);
    return symbol_duration_ms > 16.0;
}

size_t expected_payload_symbols(const lora::rx::gr::LocalHeader& hdr,
                                uint32_t sf,
                                bool ldro) {
    int sf_eff = static_cast<int>(sf) - (ldro ? 2 : 0);
    if (sf_eff <= 0) return 0;

    int numerator = static_cast<int>(8 * hdr.payload_len);
    numerator -= 4 * static_cast<int>(sf);
    numerator += 28;
    if (hdr.has_crc) numerator += 16;
    if (numerator < 0) numerator = 0;

    int denom = 4 * sf_eff;
    if (denom <= 0) return 0;

    uint32_t cr_app = static_cast<uint32_t>(hdr.cr);
    if (numerator == 0) return 0;

    size_t codewords = static_cast<size_t>((numerator + denom - 1) / denom);
    return codewords * static_cast<size_t>(cr_app + 4u);
}

std::optional<PreambleDetectResult> detect_preamble_dynamic(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    size_t min_syms,
    const std::vector<int>& candidates) {
    std::vector<int> os_list = candidates;
    if (os_list.empty()) os_list = {1, 2, 4, 8};
    std::sort(os_list.begin(), os_list.end(), std::greater<int>());
    os_list.erase(std::unique(os_list.begin(), os_list.end()), os_list.end());

    auto compute_score = [&](const std::vector<std::complex<float>>& decim,
                             size_t start) -> float {
        Workspace score_ws;
        score_ws.init(sf);
        uint32_t N = score_ws.N;
        if (start + min_syms * N > decim.size()) {
            size_t available_syms = (decim.size() > start) ? ((decim.size() - start) / N) : 0u;
            if (available_syms == 0)
                return 0.f;
        }

        std::vector<std::complex<float>> conj_up(score_ws.N);
        for (uint32_t n = 0; n < score_ws.N; ++n)
            conj_up[n] = std::conj(score_ws.upchirp[n]);

        size_t usable_syms = std::min(min_syms, (decim.size() - start) / static_cast<size_t>(score_ws.N));
        if (usable_syms == 0)
            return 0.f;

        float accum = 0.f;
        for (size_t s = 0; s < usable_syms; ++s) {
            const auto* blk = &decim[start + s * score_ws.N];
            std::complex<float> corr{0.f, 0.f};
            for (uint32_t n = 0; n < score_ws.N; ++n)
                corr += blk[n] * conj_up[n];
            accum += std::abs(corr);
        }
        return accum / static_cast<float>(usable_syms);
    };

    float best_score = 0.f;
    std::optional<PreambleDetectResult> best_result;

    for (int os : os_list) {
        if (os <= 0)
            continue;

        int phase_count = std::max(os, 1);
        for (int phase = 0; phase < phase_count; ++phase) {
            std::vector<std::complex<float>> decim;
            if (os == 1 && phase == 0) {
                decim.assign(samples.begin(), samples.end());
            } else {
                decim = lora::rx::gr::decimate_os_phase(samples, os, phase);
            }

            Workspace detect_ws;
            auto pos = lora::rx::gr::detect_preamble(detect_ws,
                                                     std::span<const std::complex<float>>(decim.data(), decim.size()),
                                                     sf,
                                                     min_syms);
            if (!pos)
                continue;

            float score = compute_score(decim, *pos);
            if (score <= best_score)
                continue;

            size_t start_raw = (*pos) * static_cast<size_t>(os) + static_cast<size_t>(phase);
            unsigned int L = static_cast<unsigned int>(std::max(os * 32, os * 8));
            size_t gd_raw = static_cast<size_t>(L / 2);
            size_t adj_raw = start_raw > gd_raw ? (start_raw - gd_raw) : 0u;
            best_score = score;
            best_result = PreambleDetectResult{adj_raw, os, phase};
        }
    }

    return best_result;
}

uint8_t decode_hamming_codeword(uint16_t cw, uint32_t cw_len, uint32_t cr_app) {
    static const auto tables = lora::rx::gr::make_hamming_tables();

    auto to_code_rate = [](uint32_t cr) -> std::optional<lora::rx::gr::CodeRate> {
        switch (cr) {
            case 1: return lora::rx::gr::CodeRate::CR45;
            case 2: return lora::rx::gr::CodeRate::CR46;
            case 3: return lora::rx::gr::CodeRate::CR47;
            case 4: return lora::rx::gr::CodeRate::CR48;
            default: return std::nullopt;
        }
    };

    auto code_rate = to_code_rate(cr_app);
    if (!code_rate)
        return 0u;

    uint16_t cw_masked = static_cast<uint16_t>(cw & ((cw_len >= 16u) ? 0xFFFFu : ((1u << cw_len) - 1u)));

    auto decoded = lora::rx::gr::hamming_decode4(
        cw_masked,
        static_cast<uint8_t>(cw_len),
        *code_rate,
        tables);

#ifdef LORA_PIPELINE_DEBUG
    if (cw_len == 6 && cr_app == 2 && (cw_masked == 0x28u || cw_masked == 0x2Eu || cw_masked == 0x0Du || cw_masked == 0x20u)) {
        std::cout << "[decode_hamming_codeword] cw=0x" << std::hex << cw_masked
                  << " decoded=" << (decoded ? static_cast<int>(decoded->first & 0x0Fu) : -1)
                  << std::dec << std::endl;
    }
#endif

    auto reverse_nibble = [](uint8_t v) {
        v &= 0x0Fu;
        uint8_t r = static_cast<uint8_t>(((v & 0x1u) << 3) |
                                         ((v & 0x2u) << 1) |
                                         ((v & 0x4u) >> 1) |
                                         ((v & 0x8u) >> 3));
        return static_cast<uint8_t>(r & 0x0Fu);
    };

    if (decoded) {
        return reverse_nibble(decoded->first);
    }

    // Fall back to the nearest codeword to remain debuggable when errors exceed
    // the correction capability.
    uint8_t best_nibble = 0u;
    int best_distance = static_cast<int>(cw_len) + 1;
    for (uint32_t nib = 0; nib < 16; ++nib) {
        auto enc = lora::rx::gr::hamming_encode4(static_cast<uint8_t>(nib), *code_rate, tables);
        uint16_t ref = static_cast<uint16_t>(enc.first & ((cw_len >= 16u) ? 0xFFFFu : ((1u << cw_len) - 1u)));
        int distance = std::popcount(static_cast<unsigned>(ref ^ cw_masked));
        if (distance < best_distance) {
            best_distance = distance;
            best_nibble = reverse_nibble(static_cast<uint8_t>(nib));
        }
    }
    return best_nibble & 0x0Fu;
}

std::optional<size_t> locate_header_start(
    Workspace& ws,
    std::span<const std::complex<float>> compensated,
    size_t search_begin,
    size_t search_end,
    uint32_t N,
    std::span<const uint32_t> pattern) {
    if (pattern.empty()) return std::nullopt;
    if (search_begin >= compensated.size()) return std::nullopt;
    search_end = std::min(search_end, compensated.size());
    if (search_end <= search_begin) return std::nullopt;
    size_t needed = pattern.size() * static_cast<size_t>(N);
    if (search_begin + needed > compensated.size()) return std::nullopt;

    size_t best_cand = 0;
    size_t best_mismatches = pattern.size() + 1;

    for (size_t cand = search_begin; cand + needed <= search_end; ++cand) {
        size_t mismatches = 0;
        for (size_t s = 0; s < pattern.size(); ++s) {
            size_t idx = cand + s * static_cast<size_t>(N);
            if (idx + N > compensated.size()) {
                mismatches = pattern.size();
                break;
            }
            uint32_t raw = lora::rx::gr::demod_symbol_peak(ws, &compensated[idx]) & (N - 1u);
            if (raw != (pattern[s] & (N - 1u))) {
                ++mismatches;
                if (mismatches >= best_mismatches)
                    break;
            }
        }
        if (mismatches < best_mismatches) {
            best_mismatches = mismatches;
            best_cand = cand;
            if (best_mismatches == 0)
                break;
        }
    }

    if (best_mismatches <= pattern.size())
        return best_cand;
    return std::nullopt;
}

struct SingleFrameResult {
    bool success = false;
    std::string failure_reason;
    std::vector<uint8_t> payload;
    bool crc_ok = false;
    size_t frame_start = 0;
    size_t frame_length = 0;
};

SingleFrameResult decode_single_frame(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    const Config& cfg,
    lora::rx::gr::Crc16Ccitt& crc16) {
    
    SingleFrameResult result;
    
    std::vector<uint32_t> hdr_pattern;
    std::vector<uint8_t> hdr_nibbles;
    auto header_opt = lora::rx::gr::decode_header_with_preamble_cfo_sto_os(
        ws, samples, cfg.sf, lora::rx::gr::CodeRate::CR45,
        cfg.min_preamble_syms, cfg.expected_sync_word,
        &hdr_pattern, &hdr_nibbles);

    if (!header_opt || hdr_pattern.size() != 16 || hdr_nibbles.size() != 10) {
        result.failure_reason = "header_decode_failed";
        return result;
    }

    // Debug: Show payload decoding details
    auto header = *header_opt;
    std::cout << "DEBUG: Header decoded - payload_len=" << (int)header.payload_len 
              << ", cr=" << (int)header.cr << ", has_crc=" << header.has_crc << std::endl;

    uint32_t N = ws.N;

    auto det = detect_preamble_dynamic(ws, samples, cfg.sf, cfg.min_preamble_syms, cfg.os_candidates);
    if (!det) {
        result.failure_reason = "preamble_not_found";
        return result;
    }

    auto decimated = lora::rx::gr::decimate_os_phase(samples, det->os, det->phase);
    if (decimated.empty()) {
        result.failure_reason = "decimation_failed";
        return result;
    }

    size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
    if (start_decim >= decimated.size()) {
        result.failure_reason = "preamble_index_oob";
        return result;
    }

    auto aligned0 = std::span<const std::complex<float>>(decimated.data() + start_decim,
                                                         decimated.size() - start_decim);
    auto refined = lora::rx::gr::detect_preamble(ws, aligned0, cfg.sf, cfg.min_preamble_syms);
    if (!refined) {
        result.failure_reason = "preamble_refine_failed";
        return result;
    }
    size_t preamble_start = start_decim + *refined;
    if (preamble_start + cfg.min_preamble_syms * N > decimated.size()) {
        result.failure_reason = "insufficient_preamble_samples";
        return result;
    }

    auto cfo = lora::rx::gr::estimate_cfo_from_preamble(ws, decimated, cfg.sf, preamble_start, cfg.min_preamble_syms);
    if (!cfo) {
        result.failure_reason = "cfo_estimation_failed";
        return result;
    }

    std::vector<std::complex<float>> compensated(decimated.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
    for (size_t n = 0; n < decimated.size(); ++n) {
        float ang = two_pi_eps * static_cast<float>(n);
        compensated[n] = decimated[n] * std::complex<float>(std::cos(ang), std::sin(ang));
    }

    int sto_search = cfg.sto_search > 0 ? cfg.sto_search : static_cast<int>(N / 8);
    auto sto = lora::rx::gr::estimate_sto_from_preamble(ws, std::span<const std::complex<float>>(compensated.data(), compensated.size()), cfg.sf, preamble_start,
                                                        cfg.min_preamble_syms, sto_search);
    if (!sto) {
        result.failure_reason = "sto_estimation_failed";
        return result;
    }

    size_t aligned_start = preamble_start;
    if (*sto >= 0)
        aligned_start += static_cast<size_t>(*sto);
    else {
        size_t shift = static_cast<size_t>(-*sto);
        aligned_start = (shift > aligned_start) ? 0u : (aligned_start - shift);
    }

    if (aligned_start >= compensated.size()) {
        result.failure_reason = "aligned_start_out_of_range";
        return result;
    }

    size_t nominal_header = aligned_start + cfg.min_preamble_syms * N +
                            static_cast<size_t>(cfg.symbols_after_preamble * static_cast<float>(N));
    size_t search_begin = (nominal_header > 4u * N) ? (nominal_header - 4u * N) : 0u;
    size_t search_end = compensated.size();

    auto header_start_opt = locate_header_start(
        ws, std::span<const std::complex<float>>(compensated.data(), compensated.size()),
        search_begin, search_end, N,
        std::span<const uint32_t>(hdr_pattern.data(), hdr_pattern.size()));

    if (!header_start_opt) {
        result.failure_reason = "header_alignment_failed";
        return result;
    }

    size_t header_start = *header_start_opt;
    bool ldro = determine_ldro(cfg);
    size_t payload_symbols_per_frame = expected_payload_symbols(header, cfg.sf, ldro);
    size_t symbols_per_frame = cfg.header_symbol_count + payload_symbols_per_frame;
    size_t frame_samples_needed = symbols_per_frame * N;

    std::cout << "DEBUG: Multi-frame decoding - payload_symbols_per_frame=" << payload_symbols_per_frame
              << ", symbols_per_frame=" << symbols_per_frame
              << ", samples_per_frame=" << frame_samples_needed << std::endl;

    if (payload_symbols_per_frame == 0 || frame_samples_needed == 0) {
        result.failure_reason = "invalid_frame_geometry";
        return result;
    }

    size_t available = (compensated.size() > header_start) ? (compensated.size() - header_start) : 0u;
    size_t max_frames = available / frame_samples_needed;

    std::cout << "DEBUG: Available samples=" << available
              << ", max_frames=" << max_frames << std::endl;

    if (max_frames == 0) {
        size_t available_symbols = available / N;
        uint32_t cr_app = static_cast<uint32_t>(header.cr);
        size_t cw_len = static_cast<size_t>(cr_app + 4u);
        if (available_symbols > cfg.header_symbol_count && cw_len > 0) {
            size_t payload_symbols_available = available_symbols - cfg.header_symbol_count;
            size_t payload_symbols_aligned = (payload_symbols_available / cw_len) * cw_len;
            if (payload_symbols_aligned > 0) {
                std::cout << "DEBUG: Clamping payload symbols from " << payload_symbols_per_frame
                          << " to " << payload_symbols_aligned
                          << " based on available data (" << available_symbols
                          << " symbols, cw_len=" << cw_len << ")" << std::endl;
                payload_symbols_per_frame = payload_symbols_aligned;
                frame_samples_needed = (cfg.header_symbol_count + payload_symbols_per_frame) * N;
                max_frames = available / frame_samples_needed;
                std::cout << "DEBUG: Adjusted samples_per_frame=" << frame_samples_needed
                          << ", max_frames=" << max_frames << std::endl;
            }
        }

        if (max_frames == 0) {
            result.failure_reason = "insufficient_samples_for_frames";
            return result;
        }
    }

    // Decode multiple frames
    size_t current_offset = header_start;

    for (size_t frame_idx = 0; frame_idx < max_frames; ++frame_idx) {
        std::cout << "DEBUG: Decoding frame " << frame_idx << " at offset " << current_offset << std::endl;

        if (current_offset + frame_samples_needed > compensated.size()) {
            std::cout << "DEBUG: Not enough samples for frame " << frame_idx << std::endl;
            break;
        }

        std::vector<std::complex<float>> frame_samples(compensated.begin() + current_offset,
                                                       compensated.begin() + current_offset + frame_samples_needed);
        size_t nsym_total = frame_samples.size() / N;
        if (nsym_total == 0) {
            std::cout << "DEBUG: Frame " << frame_idx << " has no symbols" << std::endl;
            break;
        }

        std::vector<uint32_t> raw_bins(nsym_total);
        for (size_t s = 0; s < nsym_total; ++s) {
            const std::complex<float>* block = frame_samples.data() + s * N;
            for (uint32_t n = 0; n < N; ++n)
                ws.rxbuf[n] = block[n] * ws.downchirp[n];
            ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
            uint32_t max_bin = 0u;
            float max_mag = 0.f;
            for (uint32_t k = 0; k < N; ++k) {
                float mag = std::norm(ws.fftbuf[k]);
                if (mag > max_mag) {
                    max_mag = mag;
                    max_bin = k;
                }
            }
            raw_bins[s] = max_bin;
            
            // Debug: Show FFT results for first few symbols
            if (s < 10) {
                std::cout << "DEBUG: Frame[" << frame_idx << "] FFT[" << s << "] max_bin=" << max_bin << " max_mag=" << max_mag << std::endl;
            }
        }

        // Process this frame
        if (raw_bins.size() < cfg.header_symbol_count) {
            std::cout << "DEBUG: Frame " << frame_idx << " has insufficient header symbols" << std::endl;
            break;
        }
        size_t payload_symbol_count = raw_bins.size() - cfg.header_symbol_count;
        if (payload_symbol_count == 0) {
            std::cout << "DEBUG: Frame " << frame_idx << " has no payload symbols" << std::endl;
            break;
        }
        if (payload_symbol_count < payload_symbols_per_frame) {
            std::cout << "DEBUG: Frame " << frame_idx << " missing payload symbols (have "
                      << payload_symbol_count << ", need " << payload_symbols_per_frame << ")" << std::endl;
            break;
        }
        payload_symbol_count = payload_symbols_per_frame;

        uint32_t sf_app = ldro ? (cfg.sf - 2u) : cfg.sf;
        if (sf_app == 0u) {
            std::cout << "DEBUG: Frame " << frame_idx << " has invalid sf_app" << std::endl;
            break;
        }

        std::vector<uint32_t> reduced_symbols(payload_symbol_count);
        uint32_t mask = (sf_app >= 32u) ? 0xFFFFFFFFu : ((1u << sf_app) - 1u);
        
        std::cout << "DEBUG: Frame[" << frame_idx << "] Symbol decoding - " << payload_symbol_count << " symbols, sf_app=" << sf_app 
                  << ", mask=0x" << std::hex << mask << std::dec << std::endl;
    
        for (size_t i = 0; i < payload_symbol_count; ++i) {
            uint32_t raw = raw_bins[cfg.header_symbol_count + i] & (N - 1u);
            uint32_t adjusted = (raw + N - 1u) & (N - 1u);
            uint32_t gray_decoded = lora::rx::gr::gray_decode(adjusted);
            if (ldro) gray_decoded >>= 2;
            uint32_t natural = (gray_decoded + 1u) & mask;
            reduced_symbols[i] = natural;

            // Debug: Show first few symbols with detailed processing
            if (i < 5) {
                uint32_t original_bin = raw_bins[cfg.header_symbol_count + i];
                std::cout << "DEBUG: Frame[" << frame_idx << "] Symbol[" << i << "] FFT_bin=" << original_bin << " raw=" << raw
                          << " adjusted=" << adjusted << " gray_decoded=" << gray_decoded
                          << " natural=" << natural << " (N=" << N << ", mask=0x" << std::hex << mask << std::dec << ")" << std::endl;
            }
        }
        uint32_t cr_app = static_cast<uint32_t>(header.cr);
        uint32_t cw_len = cr_app + 4u;
        if (payload_symbol_count % cw_len != 0) {
            std::cout << "DEBUG: Frame " << frame_idx << " has misaligned payload symbols" << std::endl;
            break;
        }
        size_t blocks = payload_symbol_count / cw_len;

    std::vector<uint8_t> msb_first_bits;
    msb_first_bits.reserve(static_cast<size_t>(payload_symbol_count) * sf_app);
    std::vector<uint8_t> deinterleaved_bits;
    deinterleaved_bits.reserve(static_cast<size_t>(blocks) * sf_app * cw_len);

    auto inter_map = lora::rx::gr::make_diagonal_interleaver(sf_app, cw_len);
    std::vector<uint8_t> inter_block(inter_map.n_out);
    std::vector<uint8_t> deinter_block(inter_map.n_out);
    
    // Debug: Show interleaver mapping
    std::cout << "DEBUG: Interleaver mapping (first 20): ";
    for (size_t i = 0; i < std::min(size_t(20), inter_map.map.size()); ++i) {
        std::cout << inter_map.map[i] << " ";
    }
    std::cout << std::endl;

    std::cout << "DEBUG: Deinterleaving - " << blocks << " blocks, sf_app=" << sf_app 
              << ", cw_len=" << cw_len << std::endl;

    for (size_t blk = 0; blk < blocks; ++blk) {
        size_t symbol_offset = blk * cw_len;
        
        // Debug: Show symbols for first block
        if (blk == 0) {
            std::cout << "DEBUG: Block 0 symbols: ";
            for (uint32_t col = 0; col < cw_len; ++col) {
                std::cout << reduced_symbols[symbol_offset + col] << " ";
            }
            std::cout << std::endl;
        }
        
        for (uint32_t col = 0; col < cw_len; ++col) {
            uint32_t symbol = reduced_symbols[symbol_offset + col];
            
            // Debug: Show symbol to bits conversion for first few symbols
            if (blk == 0 && col < 3) {
                std::cout << "DEBUG: Symbol " << symbol << " -> bits: ";
                for (uint32_t row = 0; row < sf_app; ++row) {
                    uint8_t bit = static_cast<uint8_t>((symbol >> row) & 0x1u);
                    std::cout << (int)bit;
                }
                std::cout << std::endl;
            }
            
            for (uint32_t row = 0; row < sf_app; ++row) {
                uint8_t bit = static_cast<uint8_t>((symbol >> (sf_app - 1u - row)) & 0x1u);
                size_t idx = col * sf_app + row;
                inter_block[idx] = bit;
                msb_first_bits.push_back(bit);
            }
        }

        // Debug: Show inter_block before deinterleaving
        if (blk == 0) {
            std::cout << "DEBUG: Inter_block before deinterleaving: ";
            for (size_t i = 0; i < std::min(size_t(20), inter_block.size()); ++i) {
                std::cout << (int)inter_block[i];
            }
            std::cout << std::endl;
        }

        for (uint32_t dst = 0; dst < inter_map.n_out; ++dst) {
            uint32_t src = inter_map.map[dst];
            if (src < deinter_block.size())
                deinter_block[src] = inter_block[dst];
        }
        
        // Debug: Show deinter_block after deinterleaving
        if (blk == 0) {
            std::cout << "DEBUG: Deinter_block after deinterleaving: ";
            for (size_t i = 0; i < std::min(size_t(20), deinter_block.size()); ++i) {
                std::cout << (int)deinter_block[i];
            }
            std::cout << std::endl;
        }

        for (uint32_t row = 0; row < sf_app; ++row) {
            uint16_t cw = 0u;
            for (uint32_t col = 0; col < cw_len; ++col) {
                uint8_t bit = deinter_block[row * cw_len + col];
                // MSB-first for codeword construction (GNU Radio compatible)
                cw = static_cast<uint16_t>((cw << 1) | bit);
                deinterleaved_bits.push_back(bit);
            }
            
            // Debug: Show first few codewords
            if (blk == 0 && row < 5) {
                std::cout << "DEBUG: Block 0, Row " << row << " -> CW=0x" << std::hex << cw << std::dec << std::endl;
            }
        }
    }
    
    std::vector<uint8_t> nibbles;
    std::cout << "DEBUG: Hamming decoding - processing " << deinterleaved_bits.size() 
              << " bits in blocks of " << cw_len << std::endl;
    
    for (size_t i = 0; i + cw_len <= deinterleaved_bits.size(); i += cw_len) {
        uint16_t cw = 0u;
        for (uint32_t b = 0; b < cw_len; ++b)
            cw = static_cast<uint16_t>((cw << 1) | deinterleaved_bits[i + b]);
        
        uint8_t nib = decode_hamming_codeword(cw, cw_len, cr_app);
        nibbles.push_back(nib & 0x0Fu);
        
        // Debug: Show first few codewords
        if (i < 5 * cw_len) {
            std::cout << "DEBUG: CW[" << (i/cw_len) << "] = 0x" << std::hex << cw 
                      << " -> nibble 0x" << (int)(nib & 0x0Fu) << std::dec << std::endl;
        }
        
        // Debug: Show what the codewords should be for "Hello New Pipeline"
        // Expected nibbles: 4 8 6 5 6 c 6 c 6 f 2 0 4 e 6 5 7 7 2 0
        // Expected codewords for CR=1 (5 bits): need to calculate
        if (i < 5 * cw_len) {
            uint8_t expected_nibble = 0;
            if (i/cw_len == 0) expected_nibble = 4;  // 'H' = 0x48 -> nibble 4
            else if (i/cw_len == 1) expected_nibble = 8;  // 'H' = 0x48 -> nibble 8
            else if (i/cw_len == 2) expected_nibble = 6;  // 'e' = 0x65 -> nibble 6
            else if (i/cw_len == 3) expected_nibble = 5;  // 'e' = 0x65 -> nibble 5
            else if (i/cw_len == 4) expected_nibble = 6;  // 'l' = 0x6c -> nibble 6
            
            std::cout << "DEBUG: Expected nibble[" << (i/cw_len) << "] = 0x" << std::hex << (int)expected_nibble 
                      << ", got 0x" << (int)(nib & 0x0Fu) << std::dec << std::endl;
        }
    }

    size_t crc_bytes_requested = header.has_crc ? 2u : 0u;
    size_t available_bytes = nibbles.size() / 2u;
    size_t available_payload_bytes = (available_bytes > crc_bytes_requested)
        ? (available_bytes - crc_bytes_requested)
        : 0u;
    size_t effective_payload_len = std::min<size_t>(header.payload_len, available_payload_bytes);
    size_t effective_crc_bytes = (header.has_crc && available_bytes >= effective_payload_len + 2u) ? 2u : 0u;
    if (effective_payload_len != header.payload_len) {
        std::cout << "DEBUG: Truncating payload length from " << static_cast<size_t>(header.payload_len)
                  << " to " << effective_payload_len << " based on available bytes" << std::endl;
    }
    if (header.has_crc && effective_crc_bytes == 0 && crc_bytes_requested > 0 && available_bytes > 0) {
        std::cout << "DEBUG: CRC trailer incomplete (available_bytes=" << available_bytes
                  << ", expected payload_len=" << static_cast<size_t>(header.payload_len) << ")" << std::endl;
    }

    size_t needed = effective_payload_len + effective_crc_bytes;
    size_t expected_nibbles = needed * 2u;
    if (expected_nibbles > 0 && nibbles.size() > expected_nibbles)
        nibbles.resize(expected_nibbles);

    auto build_bytes = [&](bool swap) {
        std::cout << "DEBUG: Original nibbles (first 20): ";
        for (size_t i = 0; i < std::min<size_t>(20, nibbles.size()); ++i)
            std::cout << std::hex << (int)nibbles[i] << " ";
        std::cout << std::dec << std::endl;

        std::vector<uint8_t> raw_bytes((nibbles.size() + 1) / 2);
        for (size_t i = 0; i < raw_bytes.size(); ++i) {
            uint8_t a = (i * 2 < nibbles.size()) ? nibbles[i * 2] : 0u;
            uint8_t b = (i * 2 + 1 < nibbles.size()) ? nibbles[i * 2 + 1] : 0u;
            uint8_t low = swap ? b : a;
            uint8_t high = swap ? a : b;
            raw_bytes[i] = static_cast<uint8_t>((high << 4) | low);
        }

        std::cout << "DEBUG: Raw bytes (first 10): ";
        for (size_t i = 0; i < std::min<size_t>(10, raw_bytes.size()); ++i)
            std::cout << std::hex << (int)raw_bytes[i] << " ";
        std::cout << std::dec << std::endl;

        std::vector<uint8_t> bytes = raw_bytes;
        size_t whiten_len = std::min(effective_payload_len, bytes.size());
        if (whiten_len > 0) {
            auto payload_span = std::span<uint8_t>(bytes.data(), whiten_len);
            lora::rx::gr::dewhiten_payload(payload_span, 0);
        }

        std::cout << "DEBUG: Dewhitened payload bytes (first 10): ";
        for (size_t i = 0; i < std::min<size_t>(10, bytes.size()); ++i)
            std::cout << std::hex << (int)bytes[i] << " ";
        std::cout << std::dec << std::endl;

        return bytes;
    };

    auto evaluate_candidate = [&](const std::vector<uint8_t>& bytes,
                                  std::vector<uint8_t>& dewhitened_out,
                                  bool& crc_ok_out) -> bool {
        if (bytes.size() < needed) return false;

        dewhitened_out.assign(bytes.begin(), bytes.begin() + effective_payload_len);

        // Debug: Show dewhitened payload (dewhitening already done in build_bytes)
        std::cout << "DEBUG: Dewhitened payload (first 10 bytes): ";
        for (size_t i = 0; i < std::min(size_t(10), dewhitened_out.size()); ++i) {
            std::cout << std::hex << (int)dewhitened_out[i] << " ";
        }
        std::cout << std::dec << std::endl;

        crc_ok_out = true;
        if (effective_crc_bytes == 2u && cfg.expect_payload_crc) {
            uint16_t crc_calc = crc16.compute(dewhitened_out.data(), dewhitened_out.size());
            uint16_t crc_rx = static_cast<uint16_t>(bytes[effective_payload_len]) |
                              (static_cast<uint16_t>(bytes[effective_payload_len + 1]) << 8);
            crc_ok_out = (crc_calc == crc_rx);

            std::cout << "DEBUG: CRC calc=" << std::hex << crc_calc
                      << ", CRC rx=" << crc_rx << ", CRC OK=" << crc_ok_out << std::dec << std::endl;
        } else if (header.has_crc && cfg.expect_payload_crc) {
            crc_ok_out = false;
        }
        return crc_ok_out;
    };

    std::vector<uint8_t> candidate_payload;
    bool crc_ok = false;

    auto bytes_default = build_bytes(false);
    if (evaluate_candidate(bytes_default, candidate_payload, crc_ok) && crc_ok) {
        result.payload = std::move(candidate_payload);
        result.crc_ok = true;
        result.success = true;
        result.frame_start = det->start_sample;
        result.frame_length = frame_samples_needed;
        return result;
    } else {
        auto bytes_swapped = build_bytes(true);
        std::vector<uint8_t> payload_swapped;
        bool crc_ok_swapped = false;
        if (evaluate_candidate(bytes_swapped, payload_swapped, crc_ok_swapped) && crc_ok_swapped) {
            result.payload = std::move(payload_swapped);
            result.crc_ok = true;
            result.success = true;
            result.frame_start = det->start_sample;
            result.frame_length = frame_samples_needed;
            return result;
        } else {
            result.payload = std::move(candidate_payload);
            result.crc_ok = crc_ok;
            result.success = true;  // Accept even with CRC failure for debugging
            result.frame_start = det->start_sample;
            result.frame_length = frame_samples_needed;
            return result;
        }
    }

    return result;
}
}

} // namespace

GnuRadioLikePipeline::GnuRadioLikePipeline(Config cfg)
    : cfg_(std::move(cfg)),
      crc16_{0x1021, 0x0000, 0x0000, false, false} {}

PipelineResult GnuRadioLikePipeline::run(std::span<const std::complex<float>> samples) {
    PipelineResult result;
    
    // Try to decode multiple frames
    size_t current_offset = 0;
    size_t max_frames = 10;  // Limit to prevent infinite loops
    size_t frame_count = 0;
    
    std::vector<std::vector<uint8_t>> all_frame_payloads;
    std::vector<bool> all_frame_crc_ok;
    bool any_success = false;
    
    while (current_offset < samples.size() && frame_count < max_frames) {
        auto remaining_samples = samples.subspan(current_offset);
        
        std::cout << "Attempting to decode frame " << frame_count 
                  << " starting at offset " << current_offset 
                  << " with " << remaining_samples.size() << " samples" << std::endl;
        
        SingleFrameResult frame_result = decode_single_frame(ws_, remaining_samples, cfg_, crc16_);
        
        if (!frame_result.success) {
            std::cout << "No frame found at offset " << current_offset << std::endl;
            break;
        }
        
        any_success = true;
        std::cout << "Frame " << frame_count << " decoded successfully: " 
                  << frame_result.payload.size() << " bytes, CRC=" 
                  << (frame_result.crc_ok ? "OK" : "FAIL") << std::endl;
        
        all_frame_payloads.push_back(frame_result.payload);
        all_frame_crc_ok.push_back(frame_result.crc_ok);
        
        // Move to next frame - advance by the actual frame length
        size_t frame_advance = frame_result.frame_start + frame_result.frame_length;
        std::cout << "Advancing by " << frame_advance << " samples (start=" 
                  << frame_result.frame_start << ", length=" 
                  << frame_result.frame_length << ")" << std::endl;
        
        current_offset += frame_advance;
        frame_count++;
        
        // If we found a frame, update the main result with the first frame's sync info
        if (frame_count == 1) {
            result.frame_sync.detected = true;
            result.frame_sync.preamble_start_sample = frame_result.frame_start;
            result.frame_sync.os = 1;
            result.frame_sync.phase = 0;
            result.frame_sync.cfo = 0.0f;
            result.frame_sync.sto = 0;
            result.frame_sync.sync_detected = true;
            result.frame_sync.sync_start_sample = frame_result.frame_start;
            result.frame_sync.aligned_start_sample = frame_result.frame_start;
            result.frame_sync.header_start_sample = frame_result.frame_start;
        }
    }
    
    if (any_success) {
        result.success = true;
        
        // Combine all frame payloads into one large payload for compatibility
        std::vector<uint8_t> combined_payload;
        for (const auto& frame_payload : all_frame_payloads) {
            combined_payload.insert(combined_payload.end(), frame_payload.begin(), frame_payload.end());
        }
        result.payload.dewhitened_payload = std::move(combined_payload);
        
        // Set CRC OK if all frames have valid CRC
        result.payload.crc_ok = std::all_of(all_frame_crc_ok.begin(), all_frame_crc_ok.end(), [](bool crc) { return crc; });
        
        // Store individual frame info for debugging
        result.frame_count = frame_count;
        result.individual_frame_payloads = std::move(all_frame_payloads);
        result.individual_frame_crc_ok = std::move(all_frame_crc_ok);
    } else {
        result.success = false;
        result.failure_reason = "no_frames_decoded";
    }
    
    return result;
}

} // namespace lora::rx::pipeline
