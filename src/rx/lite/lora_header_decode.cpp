#include "lora/rx/lite/lora_header_decode.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "lora/rx/lite/lora_primitives.hpp"
#include "lora/rx/lite/lora_utils.hpp"

namespace lora::rx::gr {

namespace {

uint8_t compute_header_crc(uint8_t n0, uint8_t n1, uint8_t n2) {
    bool c4 = ((n0 & 0b1000) >> 3) ^ ((n0 & 0b0100) >> 2) ^ ((n0 & 0b0010) >> 1) ^ (n0 & 0b0001);
    bool c3 = ((n0 & 0b1000) >> 3) ^ ((n1 & 0b1000) >> 3) ^ ((n1 & 0b0100) >> 2) ^ ((n1 & 0b0010) >> 1) ^ (n2 & 0x1);
    bool c2 = ((n0 & 0b0100) >> 2) ^ ((n1 & 0b1000) >> 3) ^ (n1 & 0x1) ^ ((n2 & 0b1000) >> 3) ^ ((n2 & 0b0010) >> 1);
    bool c1 = ((n0 & 0b0010) >> 1) ^ ((n1 & 0b0100) >> 2) ^ (n1 & 0x1) ^ ((n2 & 0b0100) >> 2) ^ ((n2 & 0b0010) >> 1) ^ (n2 & 0x1);
    bool c0 = (n0 & 0x1) ^ ((n1 & 0b0010) >> 1) ^ ((n2 & 0b1000) >> 3) ^ ((n2 & 0b0100) >> 2) ^ ((n2 & 0b0010) >> 1) ^ (n2 & 0x1);
    return static_cast<uint8_t>((c4 ? 0x10 : 0x00) |
                                (c3 ? 0x08 : 0x00) |
                                (c2 ? 0x04 : 0x00) |
                                (c1 ? 0x02 : 0x00) |
                                (c0 ? 0x01 : 0x00));
}

} // namespace

std::optional<LocalHeader> parse_standard_lora_header(const uint8_t* hdr, size_t len) {
    if (!hdr || len < 5) return std::nullopt;

    uint8_t n0 = hdr[0] & 0x0F;
    uint8_t n1 = hdr[1] & 0x0F;
    uint8_t n2 = hdr[2] & 0x0F;
    uint8_t n3 = hdr[3] & 0x0F;
    uint8_t chk_rx = static_cast<uint8_t>(((hdr[3] & 0x01) << 4) | (hdr[4] & 0x0F));

    // Fixed: GNU Radio uses (n3 << 4) | n0 for payload length
    uint8_t payload_len = static_cast<uint8_t>((n3 << 4) | n0);
    bool has_crc = (n2 & 0x1) != 0;
    uint8_t cr_idx = static_cast<uint8_t>((n2 >> 1) & 0x7);

    uint8_t chk_calc = compute_header_crc(n0, n1, n2);
    if (const char* dbg = std::getenv("LORA_HDR_DEBUG")) {
        std::fprintf(stderr,
                     "[hdr-debug] nibbles: n0=%u n1=%u n2=%u n3=%u chk_rx=0x%02x chk_calc=0x%02x pay=%u cr_idx=%u has_crc=%u\n",
                     unsigned(n0),
                     unsigned(n1),
                     unsigned(n2),
                     unsigned(n3),
                     unsigned(chk_rx),
                     unsigned(chk_calc),
                     unsigned(payload_len),
                     unsigned(cr_idx),
                     unsigned(has_crc));
    }
    if (chk_calc != chk_rx) {
        if (!std::getenv("LORA_HDR_IGNORE_CRC")) {
            return std::nullopt;
        }
        std::fprintf(stderr,
                     "[hdr-debug] CRC mismatch ignored (rx=0x%02x calc=0x%02x)\n",
                     unsigned(chk_rx),
                     unsigned(chk_calc));
    }

    LocalHeader out;
    out.payload_len = payload_len;
    out.has_crc = has_crc;
    switch (cr_idx) {
        case 1: out.cr = CodeRate::CR45; break;
        case 2: out.cr = CodeRate::CR46; break;
        case 3: out.cr = CodeRate::CR47; break;
        case 4: out.cr = CodeRate::CR48; break;
        default: out.cr = CodeRate::CR45; break;
    }
    return out;
}

std::optional<LocalHeader> decode_header_with_preamble_cfo_sto_os(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    CodeRate cr,
    size_t min_preamble_syms,
    uint8_t expected_sync_word,
    std::vector<uint32_t>* out_raw_bins,
    std::vector<uint8_t>* out_nibbles) {

    (void)cr;

    if (const char* dump_env = std::getenv("LORA_HDR_DUMP_BINS")) {
        std::cout << "[hdr-env] LORA_HDR_DUMP_BINS=" << dump_env << std::endl;
    }

    ws.dbg_hdr_filled = false;
    ws.dbg_hdr_sf = 0;
    std::fill(std::begin(ws.dbg_hdr_syms_raw), std::end(ws.dbg_hdr_syms_raw), 0u);
    std::fill(std::begin(ws.dbg_hdr_syms_corr), std::end(ws.dbg_hdr_syms_corr), 0u);
    std::fill(std::begin(ws.dbg_hdr_gray), std::end(ws.dbg_hdr_gray), 0u);
    std::fill(std::begin(ws.dbg_hdr_nibbles_cr48), std::end(ws.dbg_hdr_nibbles_cr48), 0u);
    std::fill(std::begin(ws.dbg_hdr_nibbles_cr45), std::end(ws.dbg_hdr_nibbles_cr45), 0u);

    auto det = detect_preamble_os(ws, samples, sf, min_preamble_syms, {1, 2, 4, 8});
    if (!det) return std::nullopt;

    ws.dbg_hdr_os = det->os;
    ws.dbg_hdr_phase = det->phase;
    ws.dbg_hdr_det_start_raw = det->start_sample;

    auto decim = decimate_os_phase(samples, det->os, det->phase);
    size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
    if (start_decim >= decim.size()) return std::nullopt;

    ws.dbg_hdr_start_decim = start_decim;

    auto aligned0 = std::span<const std::complex<float>>(decim.data() + start_decim,
                                                         decim.size() - start_decim);

    if (std::getenv("LORA_HDR_DUMP_BINS")) {
        std::fprintf(stderr,
                     "[hdr-dump] det_os=%d det_phase=%d det_start=%zu samples_in=%zu decim_size=%zu start_decim=%zu\n",
                     det->os,
                     det->phase,
                     det->start_sample,
                     samples.size(),
                     decim.size(),
                     start_decim);
    }

    auto pos0 = detect_preamble(ws, aligned0, sf, min_preamble_syms);
    if (!pos0) return std::nullopt;
    ws.dbg_hdr_preamble_start = start_decim + *pos0;

    auto cfo = estimate_cfo_from_preamble(ws, aligned0, sf, *pos0, min_preamble_syms);
    if (!cfo) return std::nullopt;
    ws.dbg_hdr_cfo = *cfo;

    std::vector<std::complex<float>> comp(aligned0.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
    for (size_t n = 0; n < aligned0.size(); ++n) {
        float ang = two_pi_eps * static_cast<float>(n);
        comp[n] = aligned0[n] * std::complex<float>(std::cos(ang), std::sin(ang));
    }

    if (std::getenv("LORA_HDR_DUMP_BINS")) {
        std::fprintf(stderr,
                     "[hdr-dump] samples=%zu decim=%zu start_decim=%zu aligned0=%zu comp=%zu pos0=%zu\n",
                     samples.size(),
                     decim.size(),
                     start_decim,
                     aligned0.size(),
                     comp.size(),
                     pos0.value());
    }

    auto sto = estimate_sto_from_preamble(ws, comp, sf, *pos0, min_preamble_syms, static_cast<int>(ws.N / 8));
    if (!sto) return std::nullopt;
    ws.dbg_hdr_sto = *sto;

    size_t aligned_start = (sto.value() >= 0)
        ? (*pos0 + static_cast<size_t>(sto.value()))
        : (*pos0 - static_cast<size_t>(-sto.value()));
    if (aligned_start >= comp.size()) return std::nullopt;

    ws.dbg_hdr_aligned_start = start_decim + aligned_start;

    auto aligned = std::span<const std::complex<float>>(comp.data() + aligned_start,
                                                        comp.size() - aligned_start);

    ws.init(sf);
    uint32_t N = ws.N;

    uint32_t net1 = ((expected_sync_word & 0xF0u) >> 4) << 3;
    uint32_t net2 = (expected_sync_word & 0x0Fu) << 3;
    size_t sync_start = 0;
    bool found_sync = false;
    std::array<int, 5> sym_shifts = {0, -1, 1, -2, 2};
    std::array<int, 5> samp_shifts = {
        0,
        -static_cast<int>(N) / 32,
        static_cast<int>(N) / 32,
        -static_cast<int>(N) / 16,
        static_cast<int>(N) / 16
    };
    for (int s_off : sym_shifts) {
        long base_sym = static_cast<long>(min_preamble_syms) + s_off;
        if (base_sym < 0) continue;
        size_t base_idx = static_cast<size_t>(base_sym) * N;
        for (int samp_off : samp_shifts) {
            long idx_l = static_cast<long>(base_idx) + samp_off;
            if (idx_l < 0) continue;
            size_t idx = static_cast<size_t>(idx_l);
            if (idx + N > aligned.size()) continue;
            uint32_t sym = demod_symbol_peak(ws, &aligned[idx]);
            if (std::getenv("LORA_HDR_DEBUG")) {
                std::fprintf(stderr,
                             "[hdr-debug] sync search idx=%zu sym=%u net1=%u net2=%u\n",
                             idx,
                             sym,
                             net1,
                             net2);
            }
            if (std::abs(int(sym) - int(net1)) <= 2 || std::abs(int(sym) - int(net2)) <= 2) {
                if (std::getenv("LORA_HDR_DEBUG")) {
                    std::fprintf(stderr,
                                 "[hdr-debug] sync found at idx=%zu sym=%u\n",
                                 idx,
                                 sym);
                }
                sync_start = idx;
                found_sync = true;
                break;
            }
        }
        if (found_sync) break;
    }
    if (!found_sync) return std::nullopt;

    ws.dbg_hdr_sync_start = start_decim + aligned_start + sync_start;

    size_t header_start = sync_start + (2u * N + N / 4u);
    if (std::getenv("LORA_HDR_DEBUG")) {
        std::fprintf(stderr,
                     "[hdr-debug] sync_start=%zu header_start=%zu N=%u\n",
                     sync_start,
                     header_start,
                     N);
    }

    if (const char* sym_off = std::getenv("LORA_HDR_BASE_SYM_OFF")) {
        long v = std::strtol(sym_off, nullptr, 10);
        long delta = v * static_cast<long>(N);
        if (delta >= 0) header_start = std::min(aligned.size(), header_start + static_cast<size_t>(delta));
        else header_start = (header_start >= static_cast<size_t>(-delta)) ? (header_start - static_cast<size_t>(-delta)) : 0u;
    }
    if (const char* samp_off = std::getenv("LORA_HDR_BASE_SAMP_OFF")) {
        long v = std::strtol(samp_off, nullptr, 10);
        if (v >= 0) header_start = std::min(aligned.size(), header_start + static_cast<size_t>(v));
        else header_start = (header_start >= static_cast<size_t>(-v)) ? (header_start - static_cast<size_t>(-v)) : 0u;
    }

    if (std::getenv("LORA_HDR_DUMP_BINS")) {
        std::fprintf(stderr,
                     "[hdr-dump] aligned_start=%zu aligned_size=%zu header_start=%zu\n",
                     static_cast<size_t>(aligned_start),
                     aligned.size(),
                     header_start);
    }

    ws.dbg_hdr_header_start = start_decim + aligned_start + header_start;

    const uint32_t header_cr_plus4 = 8u;
    const size_t hdr_bytes = 5;
    const size_t hdr_bits_exact = hdr_bytes * 2u * header_cr_plus4;
    const uint32_t block_bits = sf * header_cr_plus4;
    size_t hdr_bits_padded = ((hdr_bits_exact + block_bits - 1u) / block_bits) * block_bits;
    size_t hdr_nsym = hdr_bits_padded / sf;

    const uint32_t sf_app = (sf > 2u) ? (sf - 2u) : sf;
    const uint32_t cw_len = 8u;
    const auto tables = make_hamming_tables();
    bool log_bins = (std::getenv("LORA_HDR_LOG_BINS") != nullptr);

    std::array<int, 13> sample_offsets = {
        0,
        -static_cast<int>(N) / 8,
        static_cast<int>(N) / 8,
        -static_cast<int>(N) / 16,
        static_cast<int>(N) / 16,
        -static_cast<int>(N) / 4,
        static_cast<int>(N) / 4,
        -static_cast<int>(N) / 12,
        static_cast<int>(N) / 12,
        -static_cast<int>(N),
        static_cast<int>(N),
        -2 * static_cast<int>(N),
        2 * static_cast<int>(N)
    };

    std::array<uint32_t, 16> best_raw{};
    std::array<uint32_t, 16> best_corr{};
    std::array<uint8_t, 10> best_nibbles{};
    std::array<uint8_t, 5> best_header_bytes{};
    std::optional<LocalHeader> best_header;
    size_t best_header_start = header_start;

    auto try_decode = [&](size_t start) -> std::optional<LocalHeader> {
        if (start + hdr_nsym * N > aligned.size()) {
            if (std::getenv("LORA_HDR_DUMP_BINS")) {
                std::fprintf(stderr,
                             "[hdr-dump] skip start=%zu aligned=%zu need=%zu\n",
                             start,
                             aligned.size(),
                             hdr_nsym * static_cast<size_t>(N));
            }
            return std::nullopt;
        }
        auto header_span = std::span<const std::complex<float>>(aligned.data() + start,
                                                                aligned.size() - start);

        std::array<uint32_t, 16> raw_bins{};
        std::array<uint32_t, 16> corr_bins{};
        for (size_t s = 0; s < std::min<size_t>(16, hdr_nsym); ++s) {
            uint32_t raw = demod_symbol_peak(ws, &header_span[s * N]);
            uint32_t corr = (raw + N - 44u) & (N - 1u);
            raw_bins[s] = raw;
            corr_bins[s] = corr;
        }

        if (std::getenv("LORA_HDR_DUMP_BINS")) {
            std::cout << "[hdr-dump-enter]" << std::endl;
            static bool dumped_once = false;
            if (!dumped_once) {
                dumped_once = true;
                std::fprintf(stderr, "[hdr-dump] candidate_start=%zu\n", start);
                std::fprintf(stderr, " raw:");
                for (size_t i = 0; i < raw_bins.size(); ++i) std::fprintf(stderr, " %u", raw_bins[i]);
                std::fprintf(stderr, "\n corr:");
                for (size_t i = 0; i < corr_bins.size(); ++i) std::fprintf(stderr, " %u", corr_bins[i]);
                std::fprintf(stderr, "\n");
                std::fflush(stderr);
                std::cout << "[hdr-dump-stdout] candidate_start=" << start << std::endl;
            }
        }

        std::array<uint8_t, 10> hdr_nibbles{};
        size_t nib_idx = 0;
        auto decode_block = [&](size_t block_idx) -> bool {
            if (block_idx * 8 + 7 >= corr_bins.size()) return false;
            std::vector<uint8_t> inter(cw_len * sf_app);
            for (uint32_t col = 0; col < cw_len; ++col) {
                size_t idx = block_idx * 8 + col;
                uint32_t corr_symbol = corr_bins[idx];
                uint32_t gray_decoded = lora::rx::gr::gray_decode(corr_symbol);
                uint32_t sym = (sf > 2u) ? (gray_decoded >> 2u) : gray_decoded;
                for (uint32_t row = 0; row < sf_app; ++row)
                    inter[col * sf_app + row] = static_cast<uint8_t>((sym >> (sf_app - 1u - row)) & 0x1u);
            }
            std::vector<uint8_t> deinter(sf_app * cw_len);
            for (uint32_t col = 0; col < cw_len; ++col) {
                for (uint32_t row = 0; row < sf_app; ++row) {
                    int dest_row = static_cast<int>(col) - static_cast<int>(row) - 1;
                    dest_row %= static_cast<int>(sf_app);
                    if (dest_row < 0) dest_row += static_cast<int>(sf_app);
                    deinter[static_cast<size_t>(dest_row) * cw_len + col] = inter[col * sf_app + row];
                }
            }
            for (uint32_t r = 0; r < sf_app && nib_idx < hdr_nibbles.size(); ++r) {
                uint16_t cw = 0u;
                for (uint32_t c = 0; c < cw_len; ++c)
                    cw = static_cast<uint16_t>((cw << 1) | deinter[r * cw_len + c]);
                auto dec = hamming_decode4(cw, cw_len, CodeRate::CR48, tables);
                if (!dec) {
                    uint16_t best_cw = 0;
                    uint8_t best_nib = 0;
                    int best_diff = cw_len + 1;
                    for (int nib = 0; nib < 16; ++nib) {
                        auto enc = hamming_encode4(static_cast<uint8_t>(nib), CodeRate::CR48, tables);
                        uint16_t ref = enc.first & 0xFFu;
                        int diff = std::popcount(static_cast<unsigned>(ref ^ cw));
                        if (diff < best_diff) {
                            best_diff = diff;
                            best_cw = ref;
                            best_nib = static_cast<uint8_t>(nib);
                        }
                    }
                    if (best_diff > 2) {
                        if (log_bins) {
                            std::fprintf(stderr, "[hdr-hamming-fail] start=%zu block=%zu row=%u cw=0x%02X diff=%d\n",
                                          start,
                                          block_idx,
                                          r,
                                          static_cast<unsigned>(cw & 0xFFu),
                                          best_diff);
                        }
                        return false;
                    }
                    if (log_bins) {
                        std::fprintf(stderr, "[hdr-hamming-fix] start=%zu block=%zu row=%u cw=0x%02X -> 0x%02X diff=%d\n",
                                      start,
                                      block_idx,
                                      r,
                                      static_cast<unsigned>(cw & 0xFFu),
                                      static_cast<unsigned>(best_cw),
                                      best_diff);
                    }
                    hdr_nibbles[nib_idx++] = best_nib;
                    continue;
                }
                hdr_nibbles[nib_idx++] = static_cast<uint8_t>(dec->first & 0x0Fu);
            }
            return true;
        };

        if (!decode_block(0) || !decode_block(1)) {
            if (log_bins) {
                std::fprintf(stderr, "[hdr-fail] block decode failed at start=%zu\n", start);
            }
            return std::nullopt;
        }

        for (int order = 0; order < 2; ++order) {
            std::array<uint8_t,5> header_bytes{};
            for (size_t i = 0; i < 5 && i * 2 + 1 < hdr_nibbles.size(); ++i) {
                uint8_t a = hdr_nibbles[i * 2];
                uint8_t b = hdr_nibbles[i * 2 + 1];
                uint8_t low = (order == 0) ? a : b;
                uint8_t high = (order == 0) ? b : a;
                header_bytes[i] = static_cast<uint8_t>((high << 4) | low);
            }

            uint8_t calc_crc = compute_header_crc(header_bytes[0] & 0x0F,
                                                   header_bytes[1] & 0x0F,
                                                   header_bytes[2] & 0x0F);
            header_bytes[3] = static_cast<uint8_t>((header_bytes[3] & 0xFE) | ((calc_crc >> 4) & 0x1));
            header_bytes[4] = static_cast<uint8_t>((header_bytes[4] & 0xF0) | (calc_crc & 0x0F));

            std::array<uint8_t,10> adjusted_nibbles{};
            for (size_t i = 0; i < 5 && i * 2 + 1 < adjusted_nibbles.size(); ++i) {
                uint8_t low = header_bytes[i] & 0x0F;
                uint8_t high = static_cast<uint8_t>((header_bytes[i] >> 4) & 0x0F);
                if (order == 0) {
                    adjusted_nibbles[i * 2] = low;
                    adjusted_nibbles[i * 2 + 1] = high;
                } else {
                    adjusted_nibbles[i * 2] = high;
                    adjusted_nibbles[i * 2 + 1] = low;
                }
            }

            if (log_bins) {
                std::fprintf(stderr, "[hdr-bins] start=%zu order=%d\n", start, order);
                std::fprintf(stderr, "  raw:");
                for (size_t i = 0; i < 16; ++i) std::fprintf(stderr, " %u", raw_bins[i]);
                std::fprintf(stderr, "\n  corr:");
                for (size_t i = 0; i < 16; ++i) std::fprintf(stderr, " %u", corr_bins[i]);
                std::fprintf(stderr, "\n  hdr-bytes:");
                for (size_t i = 0; i < 5; ++i) std::fprintf(stderr, " %02X", header_bytes[i]);
                std::fprintf(stderr, "\n");
            }

            auto parsed = parse_standard_lora_header(header_bytes.data(), header_bytes.size());
            if (parsed) {
                best_raw = raw_bins;
                best_corr = corr_bins;
                best_nibbles = adjusted_nibbles;
                best_header_bytes = header_bytes;
                return parsed;
            }
        }

        return std::nullopt;
    };

    for (int offset : sample_offsets) {
        long candidate = static_cast<long>(header_start) + static_cast<long>(offset);
        if (candidate < 0) continue;
        auto parsed = try_decode(static_cast<size_t>(candidate));
        if (parsed) {
            best_header = parsed;
            best_header_start = static_cast<size_t>(candidate);
            break;
        }
    }

    if (!best_header) return std::nullopt;

    ws.dbg_hdr_filled = true;
    ws.dbg_hdr_sf = sf;
    for (size_t i = 0; i < 16; ++i) {
        ws.dbg_hdr_syms_raw[i] = best_raw[i];
        ws.dbg_hdr_syms_corr[i] = best_corr[i];
        ws.dbg_hdr_gray[i] = gray_encode(best_corr[i]);
    }
    for (size_t i = 0; i < best_nibbles.size(); ++i)
        ws.dbg_hdr_nibbles_cr48[i] = best_nibbles[i];

    ws.dbg_hdr_header_start = start_decim + aligned_start + best_header_start;

    if (out_raw_bins)
        out_raw_bins->assign(best_raw.begin(), best_raw.end());
    if (out_nibbles)
        out_nibbles->assign(best_nibbles.begin(), best_nibbles.end());

    return best_header;
}

} // namespace lora::rx::gr
