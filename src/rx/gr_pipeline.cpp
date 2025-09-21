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
    size_t raw_frame_samples = 0;
    int detection_os = 1;
    int detection_phase = 0;
    std::optional<float> fractional_cfo;
    std::optional<int> sto;
    size_t preamble_start_sample = 0;
    size_t aligned_start_sample = 0;
    size_t header_start_sample = 0;
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

    result.detection_os = det->os;
    result.detection_phase = det->phase;

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

    std::cout << "DEBUG: Fractional CFO estimate=" << *cfo << std::endl;

    result.fractional_cfo = *cfo;

    std::vector<std::complex<float>> compensated(decimated.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
    for (size_t n = 0; n < decimated.size(); ++n) {
        float ang = two_pi_eps * static_cast<float>(n);
        compensated[n] = decimated[n] * std::complex<float>(std::cos(ang), std::sin(ang));
    }
    std::vector<std::complex<float>> demod_downchirp(ws.downchirp.begin(), ws.downchirp.end());

    int sto_search = cfg.sto_search > 0 ? cfg.sto_search : static_cast<int>(N / 8);
    auto sto = lora::rx::gr::estimate_sto_from_preamble(ws, std::span<const std::complex<float>>(compensated.data(), compensated.size()), cfg.sf, preamble_start,
                                                        cfg.min_preamble_syms, sto_search);
    if (!sto) {
        result.failure_reason = "sto_estimation_failed";
        return result;
    }

    std::cout << "DEBUG: STO estimate=" << *sto << std::endl;

    result.sto = *sto;

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
    int phase_non_negative = std::max(det->phase, 0);
    int os_positive = std::max(det->os, 1);
    size_t os_positive_sz = static_cast<size_t>(os_positive);
    auto to_raw = [&](size_t decim_idx) -> size_t {
        return static_cast<size_t>(phase_non_negative) + decim_idx * os_positive_sz;
    };
    result.preamble_start_sample = to_raw(preamble_start);
    result.aligned_start_sample = to_raw(aligned_start);
    result.header_start_sample = to_raw(header_start);
    bool ldro = determine_ldro(cfg);
    int cfo_int = 0;
    if (header_start >= N) {
        const std::complex<float>* down_block = compensated.data() + (header_start - N);
        for (uint32_t n = 0; n < N; ++n)
            ws.rxbuf[n] = down_block[n] * ws.upchirp[n];
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
        int down_val = static_cast<int>(max_bin);
        if (down_val < static_cast<int>(N / 2))
            cfo_int = down_val / 2;
        else
            cfo_int = static_cast<int>((down_val - static_cast<int>(N)) / 2);

        if (cfo_int != 0) {
            std::cout << "DEBUG: Applying integer CFO correction cfo_int=" << cfo_int << std::endl;
            float phase_step = -2.0f * static_cast<float>(M_PI) * static_cast<float>(cfo_int) / static_cast<float>(N);
            for (uint32_t n = 0; n < N; ++n) {
                float ang = phase_step * static_cast<float>(n);
                demod_downchirp[n] *= std::complex<float>(std::cos(ang), std::sin(ang));
            }
        }
    }

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
                ws.rxbuf[n] = block[n] * demod_downchirp[n];
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

        uint32_t cr_app = static_cast<uint32_t>(header.cr);
        size_t cw_len = static_cast<size_t>(cr_app + 4u);
        if (cw_len == 0u) {
            std::cout << "DEBUG: Frame " << frame_idx << " has invalid cw_len" << std::endl;
            break;
        }

        size_t blocks = payload_symbol_count / cw_len;
        if (blocks == 0u) {
            std::cout << "DEBUG: Frame " << frame_idx << " has no full codeword blocks (cw_len="
                      << cw_len << ", payload_symbols=" << payload_symbol_count << ")" << std::endl;
            break;
        }

        payload_symbol_count = blocks * cw_len;

        uint32_t sf_app = ldro ? (cfg.sf - 2u) : cfg.sf;
        if (sf_app == 0u) {
            std::cout << "DEBUG: Frame " << frame_idx << " has invalid sf_app" << std::endl;
            break;
        }

        auto deinterleave_block = [&](const std::vector<uint32_t>& words) {
            std::vector<uint8_t> out(sf_app, 0u);
            if (sf_app == 0u) return out;

            size_t cols = words.size();
            std::vector<std::vector<uint8_t>> inter_bin(cols, std::vector<uint8_t>(sf_app, 0u));
            for (size_t col = 0; col < cols; ++col) {
                for (uint32_t row = 0; row < sf_app; ++row) {
                    uint32_t bit_idx = sf_app - 1u - row;
                    inter_bin[col][row] = static_cast<uint8_t>((words[col] >> bit_idx) & 0x1u);
                }
            }

            std::vector<std::vector<uint8_t>> deinter_bin(sf_app, std::vector<uint8_t>(cols, 0u));
            auto mod = [](int a, int b) {
                int r = a % b;
                return static_cast<size_t>((r < 0) ? (r + b) : r);
            };

            for (size_t col = 0; col < cols; ++col) {
                for (uint32_t row = 0; row < sf_app; ++row) {
                    size_t dst_row = mod(static_cast<int>(col) - static_cast<int>(row) - 1, static_cast<int>(sf_app));
                    deinter_bin[dst_row][col] = inter_bin[col][row];
                }
            }

            for (uint32_t row = 0; row < sf_app; ++row) {
                uint8_t value = 0u;
                for (size_t col = 0; col < cols; ++col) {
                    value = static_cast<uint8_t>((value << 1) | deinter_bin[row][col]);
                }
                out[row] = value;
            }

            return out;
        };

        std::vector<uint32_t> words_block;
        words_block.reserve(cw_len);
        std::vector<uint8_t> deinterleaved_words;
        deinterleaved_words.reserve(blocks * sf_app);

        std::cout << "DEBUG: Deinterleaving (GNU Radio compatible) - " << blocks
                  << " blocks, sf_app=" << sf_app << ", cw_len=" << cw_len << std::endl;

        bool block_failure = false;
        for (size_t blk = 0; blk < blocks; ++blk) {
            words_block.clear();
            if (blk == 0) std::cout << "DEBUG: Block 0 FFT bins -> Gray: ";
            for (uint32_t col = 0; col < cw_len; ++col) {
                size_t idx = cfg.header_symbol_count + blk * cw_len + col;
                if (idx >= raw_bins.size()) {
                    std::cout << "DEBUG: Frame " << frame_idx
                              << " index past raw_bins (idx=" << idx
                              << ", size=" << raw_bins.size() << ")" << std::endl;
                    words_block.clear();
                    block_failure = true;
                    break;
                }
                uint32_t raw = raw_bins[idx] & (N - 1u);
                uint32_t gray_decoded = lora::rx::gr::gray_decode(raw);
                gray_decoded = (gray_decoded + 1u) & (N - 1u);
                if (cfo_int != 0) {
                    uint32_t offset = static_cast<uint32_t>(((cfo_int % static_cast<int>(N)) + static_cast<int>(N)) % static_cast<int>(N));
                    gray_decoded = (gray_decoded + offset) & (N - 1u);
                }
                if (ldro) gray_decoded >>= 2;
                uint32_t mask = (sf_app >= 32u) ? 0xFFFFFFFFu : ((1u << sf_app) - 1u);
                uint32_t natural = gray_decoded & mask;
                if (blk == 0 && col < 8) {
                    std::cout << "DEBUG: Symbol " << col
                              << " raw=" << raw
                              << " gray_decoded=" << gray_decoded
                              << " natural=" << natural << std::endl;
                }
                words_block.push_back(natural);
                if (blk == 0) {
                    std::cout << natural << " ";
                }
            }
            if (block_failure || words_block.size() != cw_len) {
                std::cout << "DEBUG: Frame " << frame_idx << " truncated words_block" << std::endl;
                block_failure = true;
                break;
            }
            if (blk == 0) std::cout << std::endl;

            auto block_words = deinterleave_block(words_block);
            if (blk == 0) {
                std::cout << "DEBUG: Block 0 deinterleaved words: ";
                for (size_t i = 0; i < block_words.size(); ++i)
                    std::cout << std::hex << static_cast<int>(block_words[i]) << " ";
                std::cout << std::dec << std::endl;
            }
            deinterleaved_words.insert(deinterleaved_words.end(), block_words.begin(), block_words.end());
        }

        if (block_failure || deinterleaved_words.size() != blocks * sf_app) {
            std::cout << "DEBUG: Frame " << frame_idx
                      << " incomplete deinterleaving (have " << deinterleaved_words.size()
                      << ", expected " << (blocks * sf_app) << ")" << std::endl;
            break;
        }

        static constexpr std::array<uint8_t, 8> kShufflePattern = {5, 0, 1, 2, 4, 3, 6, 7};
        std::vector<uint8_t> deshuffled_words;
        deshuffled_words.reserve(deinterleaved_words.size());
        for (size_t i = 0; i < deinterleaved_words.size(); ++i) {
            uint8_t w = deinterleaved_words[i];
            uint8_t r = 0u;
            for (size_t j = 0; j < kShufflePattern.size(); ++j) {
                uint8_t bit = static_cast<uint8_t>((w >> kShufflePattern[j]) & 0x1u);
                r |= static_cast<uint8_t>(bit << j);
            }
            deshuffled_words.push_back(r);
        }

        std::vector<uint8_t> dewhitened_words = deshuffled_words;
        if (!dewhitened_words.empty()) {
            lora::rx::gr::dewhiten_payload(std::span<uint8_t>(dewhitened_words.data(), dewhitened_words.size()), 0);
        }

        std::vector<uint8_t> nibbles;
        std::vector<uint8_t> whitened_bytes;
        nibbles.reserve(dewhitened_words.size() * 2);
        whitened_bytes.reserve((dewhitened_words.size() + 1) / 2);

        for (size_t i = 0; i < dewhitened_words.size(); i += 2) {
            uint8_t low = decode_hamming_codeword(dewhitened_words[i], static_cast<uint32_t>(cw_len), cr_app);
            uint8_t high = (i + 1 < dewhitened_words.size())
                ? decode_hamming_codeword(dewhitened_words[i + 1], static_cast<uint32_t>(cw_len), cr_app)
                : 0u;
            nibbles.push_back(low);
            if (i + 1 < dewhitened_words.size()) nibbles.push_back(high);
            uint8_t combined = static_cast<uint8_t>(((high & 0x0Fu) << 4) | (low & 0x0Fu));
            whitened_bytes.push_back(combined);
        }

        std::vector<uint8_t> raw_bytes = whitened_bytes;
        if (!raw_bytes.empty()) {
            lora::rx::gr::dewhiten_payload(std::span<uint8_t>(raw_bytes.data(), raw_bytes.size()), 0);
        }

        size_t crc_bytes_requested = header.has_crc ? 2u : 0u;
        size_t available_bytes = raw_bytes.size();
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
        if (needed > 0 && raw_bytes.size() > needed)
            raw_bytes.resize(needed);

        if (!nibbles.empty()) {
            std::cout << "DEBUG: Nibbles (first 20): ";
            for (size_t i = 0; i < std::min<size_t>(20, nibbles.size()); ++i)
                std::cout << std::hex << static_cast<int>(nibbles[i]) << " ";
            std::cout << std::dec << std::endl;
        }

        if (!whitened_bytes.empty()) {
            std::cout << "DEBUG: Whitened bytes (first 10): ";
            for (size_t i = 0; i < std::min<size_t>(10, whitened_bytes.size()); ++i)
                std::cout << std::hex << static_cast<int>(whitened_bytes[i]) << " ";
            std::cout << std::dec << std::endl;
        }

        if (!raw_bytes.empty()) {
            std::cout << "DEBUG: Raw bytes (first 10): ";
            for (size_t i = 0; i < std::min<size_t>(10, raw_bytes.size()); ++i)
                std::cout << std::hex << static_cast<int>(raw_bytes[i]) << " ";
            std::cout << std::dec << std::endl;
        }

        std::vector<uint8_t> candidate_payload;
        bool crc_ok = true;

        if (effective_payload_len > raw_bytes.size()) {
            std::cout << "DEBUG: Insufficient bytes for payload (have " << raw_bytes.size()
                      << ", need " << effective_payload_len << ")" << std::endl;
            candidate_payload = raw_bytes;
            crc_ok = false;
        } else {
            candidate_payload.assign(raw_bytes.begin(), raw_bytes.begin() + effective_payload_len);
            if (effective_crc_bytes == 2u && raw_bytes.size() >= effective_payload_len + 2u && cfg.expect_payload_crc) {
                uint16_t crc_calc = crc16.compute(candidate_payload.data(), candidate_payload.size());
                uint16_t crc_rx = static_cast<uint16_t>(raw_bytes[effective_payload_len]) |
                                  (static_cast<uint16_t>(raw_bytes[effective_payload_len + 1]) << 8);
                crc_ok = (crc_calc == crc_rx);
                std::cout << "DEBUG: CRC calc=" << std::hex << crc_calc
                          << ", CRC rx=" << crc_rx << ", CRC OK=" << crc_ok << std::dec << std::endl;
            } else if (header.has_crc && cfg.expect_payload_crc) {
                crc_ok = false;
            }
        }

        result.payload = std::move(candidate_payload);
        result.crc_ok = crc_ok;
        result.success = true;
        result.frame_start = det->start_sample;
        result.frame_length = frame_samples_needed;
        result.raw_frame_samples = frame_samples_needed * os_positive_sz;
        return result;
    }

    result.failure_reason = "frame_decode_failed";
    result.success = false;
    return result;
}

}  // namespace

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

        size_t preamble_start_absolute = current_offset + frame_result.preamble_start_sample;
        size_t aligned_start_absolute = current_offset + frame_result.aligned_start_sample;
        size_t header_start_absolute = current_offset + frame_result.header_start_sample;

        if (frame_count == 0) {
            result.frame_sync.detected = true;
            result.frame_sync.preamble_start_sample = preamble_start_absolute;
            result.frame_sync.os = frame_result.detection_os;
            result.frame_sync.phase = frame_result.detection_phase;
            result.frame_sync.cfo = frame_result.fractional_cfo.value_or(0.0f);
            result.frame_sync.sto = frame_result.sto.value_or(0);
            result.frame_sync.sync_detected = true;
            result.frame_sync.sync_start_sample = header_start_absolute;
            result.frame_sync.aligned_start_sample = aligned_start_absolute;
            result.frame_sync.header_start_sample = header_start_absolute;
        }

        // Move to next frame - advance by the actual frame length in the raw sample domain
        size_t frame_advance = frame_result.frame_start + frame_result.raw_frame_samples;
        std::cout << "Advancing by " << frame_advance << " samples (start="
                  << frame_result.frame_start << ", raw_frame_samples="
                  << frame_result.raw_frame_samples << ", decimated_length="
                  << frame_result.frame_length << ")" << std::endl;

        current_offset += frame_advance;
        frame_count++;
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
