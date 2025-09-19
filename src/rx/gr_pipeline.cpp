#include "lora/rx/gr_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <utility>

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

    int num = static_cast<int>(8 * hdr.payload_len) - 4 * static_cast<int>(sf) + 28;
    if (hdr.has_crc) num += 16;
    if (num < 0) num = 0;

    int denom = 4 * sf_eff;
    if (denom <= 0) return 0;

    size_t codewords = (num == 0) ? 0 : static_cast<size_t>((num + denom - 1) / denom);
    uint32_t cr_app = static_cast<uint32_t>(hdr.cr);
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

    for (int os : os_list) {
        if (os <= 0) continue;
        if (os == 1) {
            if (auto pos = lora::rx::gr::detect_preamble(ws, samples, sf, min_syms))
                return PreambleDetectResult{*pos, 1, 0};
            continue;
        }
        for (int phase = 0; phase < os; ++phase) {
            auto decim = lora::rx::gr::decimate_os_phase(samples, os, phase);
            if (auto pos = lora::rx::gr::detect_preamble(ws, std::span<const std::complex<float>>(decim.data(), decim.size()), sf, min_syms)) {
                size_t start_raw = (*pos) * static_cast<size_t>(os) + static_cast<size_t>(phase);
                unsigned int L = static_cast<unsigned int>(std::max(os * 32, os * 8));
                size_t gd_raw = static_cast<size_t>(L / 2);
                size_t adj_raw = start_raw > gd_raw ? (start_raw - gd_raw) : 0u;
                return PreambleDetectResult{adj_raw, os, phase};
            }
        }
    }
    return std::nullopt;
}

constexpr std::array<uint8_t, 16> kCwLut = {
    0x00, 0x17, 0x2D, 0x3A, 0x4E, 0x59, 0x63, 0x74,
    0x8B, 0x98, 0xA2, 0xB1, 0xC5, 0xD2, 0xEC, 0xFB
};

uint8_t decode_hamming_codeword(uint16_t cw, uint32_t cw_len, uint32_t cr_app) {
    std::vector<bool> codeword(cw_len);
    for (uint32_t j = 0; j < cw_len; ++j)
        codeword[j] = ((cw >> (cw_len - 1 - j)) & 0x1u) != 0;

    std::vector<bool> data_nibble(4);
    data_nibble[0] = codeword[3];
    data_nibble[1] = codeword[2];
    data_nibble[2] = codeword[1];
    data_nibble[3] = codeword[0];

    switch (cr_app) {
        case 4: {
            if ((std::count(codeword.begin(), codeword.end(), true) % 2) == 0)
                break;
            [[fallthrough]];
        }
        case 3: {
            bool s0 = codeword[0] ^ codeword[1] ^ codeword[2] ^ codeword[4];
            bool s1 = codeword[1] ^ codeword[2] ^ codeword[3] ^ codeword[5];
            bool s2 = codeword[0] ^ codeword[1] ^ codeword[3] ^ codeword[6];
            int synd = static_cast<int>(s0) + (static_cast<int>(s1) << 1) + (static_cast<int>(s2) << 2);
            switch (synd) {
                case 5: data_nibble[3] = !data_nibble[3]; break;
                case 7: data_nibble[2] = !data_nibble[2]; break;
                case 3: data_nibble[1] = !data_nibble[1]; break;
                case 6: data_nibble[0] = !data_nibble[0]; break;
                default: break;
            }
            break;
        }
        case 2: {
            (void)(codeword[0] ^ codeword[1] ^ codeword[2] ^ codeword[4]);
            (void)(codeword[1] ^ codeword[2] ^ codeword[3] ^ codeword[5]);
            break;
        }
        case 1: {
            (void)(std::count(codeword.begin(), codeword.end(), true) % 2);
            break;
        }
        default: break;
    }

    uint8_t nib = 0;
    for (bool bit : data_nibble)
        nib = static_cast<uint8_t>((nib << 1) | (bit ? 1 : 0));
    return nib;
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

} // namespace

GnuRadioLikePipeline::GnuRadioLikePipeline(Config cfg)
    : cfg_(std::move(cfg)),
      crc16_{} {}

PipelineResult GnuRadioLikePipeline::run(std::span<const std::complex<float>> samples) {
    PipelineResult result;

    ws_.init(cfg_.sf);
    std::vector<uint32_t> hdr_pattern;
    std::vector<uint8_t> hdr_nibbles;
    auto header_opt = lora::rx::gr::decode_header_with_preamble_cfo_sto_os(
        ws_, samples, cfg_.sf, lora::rx::gr::CodeRate::CR45,
        cfg_.min_preamble_syms, cfg_.expected_sync_word,
        &hdr_pattern, &hdr_nibbles);

    if (!header_opt || hdr_pattern.size() != 16 || hdr_nibbles.size() != 10) {
        result.failure_reason = "header_decode_failed";
        return result;
    }

    ws_.init(cfg_.sf);
    uint32_t N = ws_.N;

    auto det = detect_preamble_dynamic(ws_, samples, cfg_.sf, cfg_.min_preamble_syms, cfg_.os_candidates);
    if (!det) {
        result.failure_reason = "preamble_not_found";
        return result;
    }
    result.frame_sync.detected = true;
    result.frame_sync.preamble_start_sample = det->start_sample;
    result.frame_sync.os = det->os;
    result.frame_sync.phase = det->phase;

    result.frame_sync.decimated = lora::rx::gr::decimate_os_phase(samples, det->os, det->phase);
    auto& decimated = result.frame_sync.decimated;
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
    auto refined = lora::rx::gr::detect_preamble(ws_, aligned0, cfg_.sf, cfg_.min_preamble_syms);
    if (!refined) {
        result.failure_reason = "preamble_refine_failed";
        return result;
    }
    size_t preamble_start = start_decim + *refined;
    if (preamble_start + cfg_.min_preamble_syms * N > decimated.size()) {
        result.failure_reason = "insufficient_preamble_samples";
        return result;
    }

    auto cfo = lora::rx::gr::estimate_cfo_from_preamble(ws_, decimated, cfg_.sf, preamble_start, cfg_.min_preamble_syms);
    if (!cfo) {
        result.failure_reason = "cfo_estimation_failed";
        return result;
    }
    result.frame_sync.cfo = *cfo;

    result.frame_sync.compensated.resize(decimated.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
    for (size_t n = 0; n < decimated.size(); ++n) {
        float ang = two_pi_eps * static_cast<float>(n);
        result.frame_sync.compensated[n] = decimated[n] * std::complex<float>(std::cos(ang), std::sin(ang));
    }

    int sto_search = cfg_.sto_search > 0 ? cfg_.sto_search : static_cast<int>(N / 8);
    auto sto = lora::rx::gr::estimate_sto_from_preamble(ws_, std::span<const std::complex<float>>(result.frame_sync.compensated.data(), result.frame_sync.compensated.size()), cfg_.sf, preamble_start,
                                                        cfg_.min_preamble_syms, sto_search);
    if (!sto) {
        result.failure_reason = "sto_estimation_failed";
        return result;
    }
    result.frame_sync.sto = *sto;

    size_t aligned_start = preamble_start;
    if (*sto >= 0)
        aligned_start += static_cast<size_t>(*sto);
    else {
        size_t shift = static_cast<size_t>(-*sto);
        aligned_start = (shift > aligned_start) ? 0u : (aligned_start - shift);
    }

    if (aligned_start >= result.frame_sync.compensated.size()) {
        result.failure_reason = "aligned_start_out_of_range";
        return result;
    }

    result.frame_sync.aligned_start_sample = aligned_start;

    size_t nominal_header = aligned_start + cfg_.min_preamble_syms * N +
                            static_cast<size_t>(cfg_.symbols_after_preamble * static_cast<float>(N));
    size_t search_begin = (nominal_header > 4u * N) ? (nominal_header - 4u * N) : 0u;
    size_t search_end = result.frame_sync.compensated.size();

    auto header_start_opt = locate_header_start(
        ws_, std::span<const std::complex<float>>(result.frame_sync.compensated.data(),
                                                  result.frame_sync.compensated.size()),
        search_begin, search_end, N,
        std::span<const uint32_t>(hdr_pattern.data(), hdr_pattern.size()));

    if (!header_start_opt) {
        result.failure_reason = "header_alignment_failed";
        return result;
    }

    size_t header_start = *header_start_opt;
    size_t sync_offset = 2u * N + N / 4u;
    size_t sync_start = (header_start >= sync_offset) ? (header_start - sync_offset) : 0u;

    result.frame_sync.sync_detected = true;
    result.frame_sync.sync_start_sample = sync_start;
    result.frame_sync.header_start_sample = header_start;

    size_t available = result.frame_sync.compensated.size() - header_start;
    size_t nsamp = (available / N) * N;
    if (nsamp == 0) {
        result.failure_reason = "insufficient_samples_for_header";
        return result;
    }

    result.frame_sync.frame_samples.assign(result.frame_sync.compensated.begin() + header_start,
                                           result.frame_sync.compensated.begin() + header_start + nsamp);
    size_t nsym_total = result.frame_sync.frame_samples.size() / N;
    if (nsym_total == 0) {
        result.failure_reason = "insufficient_samples_for_header";
        return result;
    }

    result.fft.raw_bins.resize(nsym_total);
    for (size_t s = 0; s < nsym_total; ++s) {
        const std::complex<float>* block = result.frame_sync.frame_samples.data() + s * N;
        for (uint32_t n = 0; n < N; ++n)
            ws_.rxbuf[n] = block[n] * ws_.downchirp[n];
        ws_.fft(ws_.rxbuf.data(), ws_.fftbuf.data());
        uint32_t max_bin = 0u;
        float max_mag = 0.f;
        for (uint32_t k = 0; k < N; ++k) {
            float mag = std::norm(ws_.fftbuf[k]);
            if (mag > max_mag) {
                max_mag = mag;
                max_bin = k;
            }
        }
        result.fft.raw_bins[s] = max_bin;
    }

    result.header.cw_bytes.resize(hdr_nibbles.size());
    for (size_t i = 0; i < hdr_nibbles.size(); ++i)
        result.header.cw_bytes[i] = kCwLut[hdr_nibbles[i] & 0x0Fu];

    result.header.decoded_nibbles = hdr_nibbles;
    result.header.header_bytes.resize(hdr_nibbles.size() / 2);
    for (size_t i = 0; i < result.header.header_bytes.size(); ++i) {
        uint8_t low = hdr_nibbles[i * 2];
        uint8_t high = hdr_nibbles[i * 2 + 1];
        result.header.header_bytes[i] = static_cast<uint8_t>((high << 4) | low);
    }
    result.header.header = header_opt;

    if (!result.header.header) {
        result.failure_reason = "header_crc_failed";
        return result;
    }

    if (!cfg_.decode_payload) {
        result.success = true;
        return result;
    }

    auto header = *result.header.header;
    uint32_t cr_plus4 = static_cast<uint32_t>(header.cr) + 4u;
    bool ldro = determine_ldro(cfg_);

    if (result.fft.raw_bins.size() < cfg_.header_symbol_count) {
        result.failure_reason = "insufficient_header_symbols";
        return result;
    }
    size_t payload_symbol_count = result.fft.raw_bins.size() - cfg_.header_symbol_count;
    if (payload_symbol_count == 0 && header.payload_len > 0) {
        result.failure_reason = "insufficient_payload_symbols";
        return result;
    }

    size_t expected_symbols = expected_payload_symbols(header, cfg_.sf, ldro);
    if (expected_symbols > 0) {
        if (payload_symbol_count < expected_symbols) {
            result.failure_reason = "insufficient_payload_symbols";
            return result;
        }
        if (payload_symbol_count > expected_symbols) {
            payload_symbol_count = expected_symbols;
        }
    }

    uint32_t sf_app = ldro ? (cfg_.sf - 2u) : cfg_.sf;
    if (sf_app == 0u) {
        result.failure_reason = "invalid_sf_app";
        return result;
    }

    result.gray.reduced_symbols.resize(payload_symbol_count);
    result.gray.gray_symbols.resize(payload_symbol_count);
    uint32_t mask = (sf_app >= 32u) ? 0xFFFFFFFFu : ((1u << sf_app) - 1u);
    for (size_t i = 0; i < payload_symbol_count; ++i) {
        uint32_t raw = result.fft.raw_bins[cfg_.header_symbol_count + i] & (N - 1u);
        uint32_t shifted = (raw + N - 1u) & (N - 1u);
        if (ldro) shifted >>= 2;
        uint32_t natural = (lora::rx::gr::gray_decode(shifted) + 1u) & mask;
        result.gray.reduced_symbols[i] = natural;
        result.gray.gray_symbols[i] = shifted;
    }
    uint32_t cr_app = static_cast<uint32_t>(header.cr);
    uint32_t cw_len = cr_app + 4u;
    if (payload_symbol_count % cw_len != 0) {
        result.failure_reason = "payload_symbol_misaligned";
        return result;
    }
    size_t blocks = payload_symbol_count / cw_len;

    result.bits.msb_first_bits.clear();
    result.bits.msb_first_bits.reserve(static_cast<size_t>(payload_symbol_count) * sf_app);
    result.bits.deinterleaved_bits.clear();
    result.bits.deinterleaved_bits.reserve(static_cast<size_t>(blocks) * sf_app * cw_len);

    auto inter_map = lora::rx::gr::make_diagonal_interleaver(sf_app, cw_len);
    std::vector<uint8_t> inter_block(inter_map.n_out);
    std::vector<uint8_t> deinter_block(inter_map.n_out);

    for (size_t blk = 0; blk < blocks; ++blk) {
        size_t symbol_offset = blk * cw_len;
        for (uint32_t col = 0; col < cw_len; ++col) {
            uint32_t symbol = result.gray.reduced_symbols[symbol_offset + col];
            for (uint32_t row = 0; row < sf_app; ++row) {
                uint8_t bit = static_cast<uint8_t>((symbol >> (sf_app - 1 - row)) & 0x1u);
                size_t idx = col * sf_app + row;
                inter_block[idx] = bit;
                result.bits.msb_first_bits.push_back(bit);
            }
        }

        for (uint32_t dst = 0; dst < inter_map.n_out; ++dst) {
            uint32_t src = inter_map.map[dst];
            if (src < deinter_block.size())
                deinter_block[src] = inter_block[dst];
        }

        for (uint32_t row = 0; row < sf_app; ++row) {
            uint16_t cw = 0u;
            for (uint32_t col = 0; col < cw_len; ++col) {
                uint8_t bit = deinter_block[row * cw_len + col];
                cw = static_cast<uint16_t>((cw << 1) | bit);
                result.bits.deinterleaved_bits.push_back(bit);
            }
        }
    }
    result.fec.nibbles.clear();
    for (size_t i = 0; i + cw_len <= result.bits.deinterleaved_bits.size(); i += cw_len) {
        uint16_t cw = 0u;
        for (uint32_t b = 0; b < cw_len; ++b)
            cw = static_cast<uint16_t>((cw << 1) | result.bits.deinterleaved_bits[i + b]);
        uint8_t nib = decode_hamming_codeword(cw, cw_len, cr_app);
        result.fec.nibbles.push_back(nib & 0x0Fu);
    }

    size_t crc_bytes = header.has_crc ? 2u : 0u;
    size_t needed = static_cast<size_t>(header.payload_len) + crc_bytes;
    size_t expected_nibbles = needed * 2u;
    if (expected_nibbles > 0 && result.fec.nibbles.size() > expected_nibbles)
        result.fec.nibbles.resize(expected_nibbles);

    auto build_bytes = [&](bool swap) {
        std::vector<uint8_t> bytes((result.fec.nibbles.size() + 1) / 2);
        for (size_t i = 0; i < bytes.size(); ++i) {
            uint8_t a = (i * 2 < result.fec.nibbles.size()) ? result.fec.nibbles[i * 2] : 0u;
            uint8_t b = (i * 2 + 1 < result.fec.nibbles.size()) ? result.fec.nibbles[i * 2 + 1] : 0u;
            uint8_t low = swap ? b : a;
            uint8_t high = swap ? a : b;
            bytes[i] = static_cast<uint8_t>((high << 4) | low);
        }
        return bytes;
    };

    auto evaluate_candidate = [&](const std::vector<uint8_t>& bytes,
                                  std::vector<uint8_t>& dewhitened_out,
                                  bool& crc_ok_out) -> bool {
        if (bytes.size() < needed) return false;

        dewhitened_out.assign(bytes.begin(), bytes.begin() + header.payload_len);
        if (!dewhitened_out.empty())
            lora::rx::gr::dewhiten_payload(dewhitened_out);

        crc_ok_out = true;
        if (header.has_crc && cfg_.expect_payload_crc) {
            uint16_t crc_calc = crc16_.compute(dewhitened_out.data(), dewhitened_out.size());
            uint16_t crc_rx = static_cast<uint16_t>(bytes[header.payload_len]) |
                              (static_cast<uint16_t>(bytes[header.payload_len + 1]) << 8);
            crc_ok_out = (crc_calc == crc_rx);
        }
        return crc_ok_out;
    };

    std::vector<uint8_t> candidate_payload;
    bool crc_ok = false;

    auto bytes_default = build_bytes(false);
    if (evaluate_candidate(bytes_default, candidate_payload, crc_ok) && crc_ok) {
        result.fec.raw_bytes = std::move(bytes_default);
    } else {
        auto bytes_swapped = build_bytes(true);
        std::vector<uint8_t> payload_swapped;
        bool crc_ok_swapped = false;
        if (evaluate_candidate(bytes_swapped, payload_swapped, crc_ok_swapped) && crc_ok_swapped) {
            result.fec.raw_bytes = std::move(bytes_swapped);
            candidate_payload = std::move(payload_swapped);
            crc_ok = true;
        } else {
            result.fec.raw_bytes = std::move(bytes_default);
            // choose whichever payload we have (default already evaluated once)
            if (!crc_ok) {
                evaluate_candidate(result.fec.raw_bytes, candidate_payload, crc_ok);
            }
        }
    }

    if (result.fec.raw_bytes.size() < needed) {
        result.failure_reason = "insufficient_payload_bytes";
        return result;
    }

    result.payload.dewhitened_payload = std::move(candidate_payload);
    result.payload.crc_ok = crc_ok;

    if (header.has_crc && cfg_.expect_payload_crc && !crc_ok) {
        result.failure_reason = "payload_crc_failed";
        return result;
    }

    result.success = true;
    return result;
}

} // namespace lora::rx::pipeline
