#include "lora/rx/frame.hpp"
#include "lora/rx/header_decode.hpp"
#include "lora/utils/gray.hpp"
#include "lora/rx/demod.hpp"
#include "lora/utils/crc.hpp"
#include "lora/utils/whitening.hpp"
#include "lora/rx/preamble.hpp"
#include "lora/debug.hpp"
#include "lora/rx/decimate.hpp"
#include <vector>
#include <complex>
#include <cstdio>

namespace lora::rx {

// Local demod helper (same as in frame.cpp)
// use demod_symbol_peak from demod.hpp

std::optional<LocalHeader> decode_header_with_preamble_cfo_sto_os_impl(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    lora::utils::CodeRate cr,
    size_t min_preamble_syms,
    uint8_t expected_sync) {
    printf("DEBUG: [impl] decode_header_with_preamble_cfo_sto_os_impl\n");
    // OS-aware detect + decimate
    auto det = detect_preamble_os(ws, samples, sf, min_preamble_syms, {4,2,1,8});
    if (!det) return std::nullopt;
    auto decim = decimate_os_phase(samples, det->os, det->phase);
    size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
    if (start_decim >= decim.size()) return std::nullopt;
    auto aligned0 = std::span<const std::complex<float>>(decim.data() + start_decim,
                                                         decim.size() - start_decim);
    // CFO
    auto pos0 = detect_preamble(ws, aligned0, sf, min_preamble_syms);
    if (!pos0) return std::nullopt;
    auto cfo = estimate_cfo_from_preamble(ws, aligned0, sf, *pos0, min_preamble_syms);
    if (!cfo) return std::nullopt;
    std::vector<std::complex<float>> comp(aligned0.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
    std::complex<float> j(0.f, 1.f);
    for (size_t n = 0; n < aligned0.size(); ++n)
        comp[n] = aligned0[n] * std::exp(j * (two_pi_eps * static_cast<float>(n)));
    // STO
    auto sto = estimate_sto_from_preamble(ws, comp, sf, *pos0, min_preamble_syms, static_cast<int>(ws.N/8));
    if (!sto) return std::nullopt;
    int shift = *sto;
    size_t aligned_start = (shift >= 0) ? (*pos0 + static_cast<size_t>(shift))
                                        : (*pos0 - static_cast<size_t>(-shift));
    if (aligned_start >= comp.size()) return std::nullopt;
    auto aligned = std::span<const std::complex<float>>(comp.data() + aligned_start,
                                                        comp.size() - aligned_start);
    ws.init(sf);
    uint32_t N = ws.N;
    // Sync search (elastic)
    uint32_t net1 = ((expected_sync & 0xF0u) >> 4) << 3;
    uint32_t net2 = (expected_sync & 0x0Fu) << 3;
    size_t sync_start = 0;
    bool found_sync = false;
    int sym_shifts[5]  = {0, -1, 1, -2, 2};
    int samp_shifts[5] = {0, -(int)N/32, (int)N/32, -(int)N/16, (int)N/16};
    for (int s : sym_shifts) {
        size_t base = (s >= 0) ? ((min_preamble_syms + (size_t)s) * N)
                               : ((min_preamble_syms - (size_t)(-s)) * N);
        for (int so : samp_shifts) {
            size_t idx;
            if (so >= 0) {
                if (base + (size_t)so + N > aligned.size()) continue;
                idx = base + (size_t)so;
            } else {
                size_t offs = (size_t)(-so);
                if (base < offs) continue;
                idx = base - offs;
                if (idx + N > aligned.size()) continue;
            }
            uint32_t ss = demod_symbol_peak(ws, &aligned[idx]);
            if (std::abs(int(ss) - int(net1)) <= 2 || std::abs(int(ss) - int(net2)) <= 2) {
                found_sync = true; sync_start = idx; break;
            }
        }
        if (found_sync) break;
    }
    if (!found_sync) return std::nullopt;
    // If second sync follows, skip one symbol
    if (sync_start + 2u * N <= aligned.size()) {
        uint32_t ss2 = demod_symbol_peak(ws, &aligned[sync_start + N]);
        if (std::abs(int(ss2) - int(net1)) <= 2 || std::abs(int(ss2) - int(net2)) <= 2)
            sync_start += N;
    }
    // Header anchor: sync + 2 downchirps + 0.25 symbol
    size_t hdr_start_base = sync_start + (2u * N + N/4u);
    const uint32_t header_cr_plus4 = 8u;
    size_t hdr_bytes = 5;
    const size_t hdr_bits_exact = hdr_bytes * 2 * header_cr_plus4;
    uint32_t block_bits = sf * header_cr_plus4;
    size_t hdr_bits_padded = hdr_bits_exact;
    if (hdr_bits_padded % block_bits) hdr_bits_padded = ((hdr_bits_padded / block_bits) + 1) * block_bits;
    size_t hdr_nsym = hdr_bits_padded / sf;
    if (hdr_start_base + hdr_nsym * N > aligned.size()) return std::nullopt;
    // Gray-coded symbol capture for diagnostics
    size_t hdr_start = hdr_start_base;
    auto data = std::span<const std::complex<float>>(aligned.data() + hdr_start, aligned.size() - hdr_start);
    ws.ensure_rx_buffers(hdr_nsym, sf, header_cr_plus4);
    auto& symbols = ws.rx_symbols;
    for (size_t s = 0; s < hdr_nsym; ++s) {
        uint32_t raw_symbol = demod_symbol_peak(ws, &data[s * N]);
        uint32_t corr = (raw_symbol + ws.N - 44u) % ws.N;
        symbols[s] = lora::utils::gray_encode(corr);
        if (s < 16) {
            ws.dbg_hdr_filled = true;
            ws.dbg_hdr_sf = sf;
            ws.dbg_hdr_syms_raw[s]  = raw_symbol;
            ws.dbg_hdr_syms_corr[s] = corr;
            ws.dbg_hdr_gray[s]      = symbols[s];
        }
    }

    // Core GR-style block mapping with bounded two-block search (fresh origin per block)
    std::optional<lora::rx::LocalHeader> hdr_opt;
    {
        static lora::utils::HammingTables Th = lora::utils::make_hamming_tables();
        const uint32_t sf_app = (sf > 2u) ? (sf - 2u) : sf;
        const uint32_t cw_len = 8u;
        auto build_block_rows = [&](const uint32_t* gnu, size_t blk_idx, uint8_t (&rows)[5][8]) {
            std::vector<std::vector<uint8_t>> inter_bin(cw_len, std::vector<uint8_t>(sf_app, 0));
            for (uint32_t i = 0; i < cw_len; ++i) {
                uint32_t full = gnu[blk_idx * cw_len + i] & (N - 1u);
                uint32_t g    = lora::utils::gray_encode(full);
                uint32_t sub  = g & ((1u << sf_app) - 1u);
                for (uint32_t j = 0; j < sf_app; ++j)
                    inter_bin[i][j] = (uint8_t)((sub >> (sf_app - 1u - j)) & 1u);
            }
            std::vector<std::vector<uint8_t>> deinter_bin(sf_app, std::vector<uint8_t>(cw_len, 0));
            for (uint32_t i = 0; i < cw_len; ++i) {
                for (uint32_t j = 0; j < sf_app; ++j) {
                    int r = static_cast<int>(i) - static_cast<int>(j) - 1;
                    r %= static_cast<int>(sf_app);
                    if (r < 0) r += static_cast<int>(sf_app);
                    deinter_bin[static_cast<size_t>(r)][i] = inter_bin[i][j];
                }
            }
            for (uint32_t r = 0; r < sf_app; ++r)
                for (uint32_t c = 0; c < cw_len; ++c)
                    rows[r][c] = deinter_bin[r][c];
        };
        auto try_parse_two_block = [&](int off0, long samp0, int off1, long samp1) -> std::optional<lora::rx::LocalHeader> {
            if (hdr_nsym < 16 || sf_app < 3) return std::nullopt;
            // Block indices
            size_t idx0;
            if (samp0 >= 0) { idx0 = hdr_start_base + static_cast<size_t>(off0) * N + static_cast<size_t>(samp0); if (idx0 + 8u*N > aligned.size()) return std::nullopt; }
            else { size_t o = static_cast<size_t>(-samp0); size_t base0 = hdr_start_base + static_cast<size_t>(off0) * N; if (base0 < o) return std::nullopt; idx0 = base0 - o; if (idx0 + 8u*N > aligned.size()) return std::nullopt; }
            size_t base1 = hdr_start_base + 8u * N + static_cast<size_t>(off1) * N;
            size_t idx1;
            if (samp1 >= 0) { idx1 = base1 + static_cast<size_t>(samp1); if (idx1 + 8u*N > aligned.size()) return std::nullopt; }
            else { size_t o = static_cast<size_t>(-samp1); if (base1 < o) return std::nullopt; idx1 = base1 - o; if (idx1 + 8u*N > aligned.size()) return std::nullopt; }
            // Demod and reduce
            uint32_t raw0[8]{}, raw1[8]{};
            for (size_t s = 0; s < 8; ++s) { raw0[s] = demod_symbol_peak(ws, &aligned[idx0 + s * N]); raw1[s] = demod_symbol_peak(ws, &aligned[idx1 + s * N]); }
            uint32_t gnu_both[16]{};
            for (size_t s = 0; s < 8; ++s) gnu_both[s]      = ((raw0[s] + N - 1u) & (N - 1u)) >> 2;
            for (size_t s = 0; s < 8; ++s) gnu_both[8 + s]  = ((raw1[s] + N - 1u) & (N - 1u)) >> 2;
            uint8_t blk0[5][8]{}, blk1[5][8]{};
            build_block_rows(gnu_both, 0, blk0);
            build_block_rows(gnu_both, 1, blk1);
            auto assemble_and_try = [&](const uint8_t b0[5][8], const uint8_t b1[5][8]) -> std::optional<lora::rx::LocalHeader> {
                uint8_t cw_local[10]{};
                for (uint32_t r = 0; r < sf_app; ++r) { uint16_t c = 0; for (uint32_t i = 0; i < 8u; ++i) c = (c << 1) | (b0[r][i] & 1u); cw_local[r] = (uint8_t)(c & 0xFF); }
                for (uint32_t r = 0; r < sf_app; ++r) { uint16_t c = 0; for (uint32_t i = 0; i < 8u; ++i) c = (c << 1) | (b1[r][i] & 1u); cw_local[sf_app + r] = (uint8_t)(c & 0xFF); }
                std::vector<uint8_t> nibb; nibb.reserve(10);
                for (int k = 0; k < 10; ++k) { auto dec = lora::utils::hamming_decode4(cw_local[k], 8u, lora::utils::CodeRate::CR48, Th); if (!dec) return std::nullopt; nibb.push_back(dec->first & 0x0F); }
                for (int order = 0; order < 2; ++order) {
                    std::vector<uint8_t> hdr_try(5);
                    for (int i = 0; i < 5; ++i) {
                        uint8_t n0 = nibb[i*2], n1 = nibb[i*2 + 1];
                        uint8_t low  = (order==0)? n0 : n1;
                        uint8_t high = (order==0)? n1 : n0;
                        hdr_try[i] = (uint8_t)((high << 4) | low);
                    }
                    if (auto okhdr = parse_standard_lora_header(hdr_try.data(), hdr_try.size()))
                        return okhdr;
                }
                return std::nullopt;
            };
            // Baseline and small variants
            if (auto ok = assemble_and_try(blk0, blk1)) return ok;
            for (uint32_t rot1 = 0; rot1 < sf_app; ++rot1) {
                uint8_t t1[5][8]{};
                for (uint32_t r = 0; r < sf_app; ++r) { uint32_t rr = (r + rot1) % sf_app; for (uint32_t c = 0; c < 8u; ++c) t1[r][c] = blk1[rr][c]; }
                if (auto ok = assemble_and_try(blk0, t1)) return ok;
                // row-reversed
                uint8_t tr[5][8]{}; for (uint32_t r = 0; r < sf_app; ++r) for (uint32_t c = 0; c < 8u; ++c) tr[r][c] = t1[sf_app - 1u - r][c];
                if (auto ok = assemble_and_try(blk0, tr)) return ok;
                // column-reversed
                uint8_t t1c[5][8]{}; for (uint32_t r = 0; r < sf_app; ++r) for (uint32_t c = 0; c < 8u; ++c) t1c[r][c] = t1[r][7u - c];
                if (auto ok = assemble_and_try(blk0, t1c)) return ok;
                uint8_t trc[5][8]{}; for (uint32_t r = 0; r < sf_app; ++r) for (uint32_t c = 0; c < 8u; ++c) trc[r][c] = tr[r][7u - c];
                if (auto ok = assemble_and_try(blk0, trc)) return ok;
            }
            return std::nullopt;
        };
        if (hdr_nsym >= 16 && sf_app >= 3) {
            std::vector<int> off0_list = {1,2,3};
            std::vector<int> off1_list = {-1,0,1,2};
            std::vector<long> samp_list = {0, (long)N/64, -(long)N/64, (long)N/32, -(long)N/32};
            for (int off0 : off0_list) for (int off1 : off1_list) for (long s0 : samp_list) for (long s1 : samp_list)
                if (auto ok = try_parse_two_block(off0, s0, off1, s1)) return ok;
        }
    }

    // Fallback C: intra-symbol bit-shift search (MSB-first) over GR-style header stream
    {
        const auto& Mshift = ws.get_interleaver(sf, header_cr_plus4);
        // Build MSB-first bitstream from gray-coded header symbols
        std::vector<uint8_t> bits_full(hdr_nsym * sf);
        size_t bixf = 0;
        for (size_t s = 0; s < hdr_nsym; ++s) {
            uint32_t sym = symbols[s];
            for (int b = static_cast<int>(sf) - 1; b >= 0; --b)
                bits_full[bixf++] = (sym >> b) & 1u;
        }
        for (uint32_t bit_shift = 1; bit_shift < sf; ++bit_shift) {
            if (bits_full.size() <= bit_shift) break;
            std::vector<uint8_t> bits_s(bits_full.begin() + bit_shift, bits_full.end());
            // Deinterleave
            std::vector<uint8_t> deint_s(bits_s.size());
            for (size_t off = 0; off + Mshift.n_in <= bits_s.size(); off += Mshift.n_in)
                for (uint32_t i = 0; i < Mshift.n_out; ++i)
                    deint_s[off + Mshift.map[i]] = bits_s[off + i];
            if (deint_s.size() < hdr_bits_exact) continue;
            // Hamming decode with CR=4/8 over exact header bits
            static lora::utils::HammingTables Tshift = lora::utils::make_hamming_tables();
            std::vector<uint8_t> nibb_s; nibb_s.reserve(hdr_bytes * 2);
            bool ok_dec = true;
            for (size_t i = 0; i < hdr_bits_exact; i += header_cr_plus4) {
                uint16_t cw = 0;
                for (uint32_t b = 0; b < header_cr_plus4; ++b)
                    cw = (cw << 1) | deint_s[i + b];
                auto dec = lora::utils::hamming_decode4(cw, header_cr_plus4, lora::utils::CodeRate::CR48, Tshift);
                if (!dec) { ok_dec = false; break; }
                nibb_s.push_back(dec->first & 0x0F);
            }
            if (!ok_dec || nibb_s.size() != hdr_bytes * 2) continue;
            std::vector<uint8_t> hdr_s(hdr_bytes);
            for (size_t i = 0; i < hdr_bytes; ++i) {
                uint8_t low  = nibb_s[i*2];
                uint8_t high = nibb_s[i*2+1];
                hdr_s[i] = static_cast<uint8_t>((high << 4) | low);
            }
            auto hs = parse_standard_lora_header(hdr_s.data(), hdr_s.size());
            if (hs) return hs;
        }
    }

    // Fallback D: small variant search over mapping/bit orders
    {
        const auto& Mv = ws.get_interleaver(sf, header_cr_plus4);
        auto try_variant = [&](int bin_offset,
                               bool use_gray_decode,
                               bool msb_first,
                               bool high_low_nibbles) -> std::optional<lora::rx::LocalHeader> {
            // Rebuild symbols with chosen bin offset and gray map
            std::vector<uint32_t> syms(hdr_nsym);
            for (size_t s = 0; s < hdr_nsym; ++s) {
                uint32_t raw_symbol = demod_symbol_peak(ws, &data[s * N]);
                uint32_t mapped = (raw_symbol + N + (uint32_t)bin_offset) % N;
                uint32_t mapped_sym = use_gray_decode ? lora::utils::gray_decode(mapped)
                                                      : lora::utils::gray_encode(mapped);
                syms[s] = mapped_sym;
            }
            std::vector<uint8_t> bitsv(hdr_nsym * sf);
            size_t bix = 0;
            for (size_t s = 0; s < hdr_nsym; ++s) {
                uint32_t sym = syms[s];
                if (msb_first) {
                    for (int b = (int)sf - 1; b >= 0; --b) bitsv[bix++] = (sym >> b) & 1u;
                } else {
                    for (uint32_t b = 0; b < sf; ++b) bitsv[bix++] = (sym >> b) & 1u;
                }
            }
            std::vector<uint8_t> deintv(bix);
            for (size_t off = 0; off < bix; off += Mv.n_in)
                for (uint32_t i = 0; i < Mv.n_out; ++i)
                    deintv[off + Mv.map[i]] = bitsv[off + i];
            static lora::utils::HammingTables T2 = lora::utils::make_hamming_tables();
            std::vector<uint8_t> nibbv; nibbv.reserve(hdr_bytes * 2);
            for (size_t i = 0; i < hdr_bits_exact; i += header_cr_plus4) {
                uint16_t cw = 0; for (uint32_t b = 0; b < header_cr_plus4; ++b) cw = (cw << 1) | deintv[i + b];
                auto dec = lora::utils::hamming_decode4(cw, header_cr_plus4, lora::utils::CodeRate::CR48, T2);
                if (!dec) return std::nullopt;
                nibbv.push_back(dec->first & 0x0F);
            }
            std::vector<uint8_t> hdrv(hdr_bytes);
            for (size_t i = 0; i < hdr_bytes; ++i) {
                uint8_t n0 = nibbv[i*2];
                uint8_t n1 = nibbv[i*2+1];
                uint8_t low  = high_low_nibbles ? n1 : n0;
                uint8_t high = high_low_nibbles ? n0 : n1;
                hdrv[i] = (uint8_t)((high << 4) | low);
            }
            auto ok = parse_standard_lora_header(hdrv.data(), hdrv.size());
            return ok;
        };

        const int bin_offsets[2] = {0, -44};
        for (int off : bin_offsets)
            for (int g = 0; g < 2; ++g)
                for (int msb = 0; msb < 2; ++msb)
                    for (int hl = 0; hl < 2; ++hl) {
                        if (auto h = try_variant(off, g==1, msb==1, hl==1))
                            return h;
                    }
    }

    // Fallback E: GNU Radio direct nibble scan using dbg_hdr_syms_raw
    {
        if (hdr_nsym >= 10) {
            std::vector<uint8_t> nib_s(hdr_nsym);
            uint32_t Nsym = (1u << sf);
            for (size_t s = 0; s < hdr_nsym; ++s) {
                uint32_t g = lora::utils::gray_encode(ws.dbg_hdr_syms_raw[s]);
                uint32_t s_bin = lora::utils::gray_decode(g);
                uint32_t gnu = ((s_bin + Nsym - 1u) % Nsym) >> 2;
                nib_s[s] = static_cast<uint8_t>(gnu & 0x0F);
            }
            for (size_t st = 0; st + 10 <= hdr_nsym; ++st) {
                std::vector<uint8_t> gn_nibbles(nib_s.begin() + st, nib_s.begin() + st + 10);
                std::vector<uint8_t> gn_hdr(5);
                // low,high order
                for (size_t i = 0; i < 5; ++i) {
                    uint8_t low  = gn_nibbles[i*2];
                    uint8_t high = gn_nibbles[i*2+1];
                    gn_hdr[i] = static_cast<uint8_t>((high << 4) | low);
                }
                if (auto hdr_opt2 = parse_standard_lora_header(gn_hdr.data(), gn_hdr.size()))
                    return hdr_opt2;
                // high,low order
                for (size_t i = 0; i < 5; ++i) {
                    uint8_t low  = gn_nibbles[i*2];
                    uint8_t high = gn_nibbles[i*2+1];
                    gn_hdr[i] = static_cast<uint8_t>((low << 4) | high);
                }
                if (auto hdr_opt2 = parse_standard_lora_header(gn_hdr.data(), gn_hdr.size()))
                    return hdr_opt2;
            }
        }
    }

    // Optional: heavy two-block scan with fine sample shifts and block1 variants (guarded)
    if (const char* scan = std::getenv("LORA_HDR_SCAN"); scan && scan[0]=='1' && scan[1]=='\0') {
        static lora::utils::HammingTables Th = lora::utils::make_hamming_tables();
        const uint32_t sf_app = (sf > 2u) ? (sf - 2u) : sf;
        const uint32_t cw_len = 8u;
        auto build_block_rows = [&](const uint32_t* gnu, size_t blk_idx, uint8_t (&rows)[5][8]) {
            std::vector<std::vector<uint8_t>> inter_bin(cw_len, std::vector<uint8_t>(sf_app, 0));
            for (uint32_t i = 0; i < cw_len; ++i) {
                uint32_t full = gnu[blk_idx * cw_len + i] & (N - 1u);
                uint32_t g    = lora::utils::gray_encode(full);
                uint32_t sub  = g & ((1u << sf_app) - 1u);
                for (uint32_t j = 0; j < sf_app; ++j)
                    inter_bin[i][j] = (uint8_t)((sub >> (sf_app - 1u - j)) & 1u);
            }
            std::vector<std::vector<uint8_t>> deinter_bin(sf_app, std::vector<uint8_t>(cw_len, 0));
            for (uint32_t i = 0; i < cw_len; ++i) {
                for (uint32_t j = 0; j < sf_app; ++j) {
                    int r = static_cast<int>(i) - static_cast<int>(j) - 1;
                    r %= static_cast<int>(sf_app);
                    if (r < 0) r += static_cast<int>(sf_app);
                    deinter_bin[static_cast<size_t>(r)][i] = inter_bin[i][j];
                }
            }
            for (uint32_t r = 0; r < sf_app; ++r)
                for (uint32_t c = 0; c < cw_len; ++c)
                    rows[r][c] = deinter_bin[r][c];
        };
        // Build block0 raw first (fixed start at hdr_start_base)
        size_t idx0 = hdr_start_base;
        if (idx0 + 8u * N <= aligned.size() && sf_app >= 3 && hdr_nsym >= 16) {
            uint32_t raw0[8]{}; for (size_t s = 0; s < 8; ++s) raw0[s] = demod_symbol_peak(ws, &aligned[idx0 + s * N]);
            // Precompute fine sample shifts candidates near ±N/64 and ±3N/64
            std::vector<long> fine_samp1;
            int base = static_cast<int>(N) / 64;
            int base3 = (3 * static_cast<int>(N)) / 64;
            for (int d = -8; d <= 8; ++d) { fine_samp1.push_back(base + d); fine_samp1.push_back(-(base) + d); }
            for (int d = -4; d <= 4; ++d) { fine_samp1.push_back(base3 + d); fine_samp1.push_back(-base3 + d); }
            fine_samp1.push_back(0);
            std::sort(fine_samp1.begin(), fine_samp1.end());
            fine_samp1.erase(std::unique(fine_samp1.begin(), fine_samp1.end()), fine_samp1.end());
            // Try off1 in a small window
            for (int off1 = 0; off1 <= 7; ++off1) {
                size_t idx1_off = idx0 + 8u * N + static_cast<size_t>(off1) * N;
                for (long samp1 : fine_samp1) {
                    size_t idx1;
                    if (samp1 >= 0) { idx1 = idx1_off + static_cast<size_t>(samp1); }
                    else { size_t o = static_cast<size_t>(-samp1); if (idx1_off < o) continue; idx1 = idx1_off - o; }
                    if (idx1 + 8u * N > aligned.size()) continue;
                    uint32_t raw1[8]{}; for (size_t s = 0; s < 8; ++s) raw1[s] = demod_symbol_peak(ws, &aligned[idx1 + s * N]);
                    for (int mode = 0; mode < 2; ++mode) {
                        uint32_t g0[8]{}, g1[8]{};
                        for (size_t s = 0; s < 8; ++s) {
                            if (mode == 0) {
                                g0[s] = ((raw0[s] + N - 1u) & (N - 1u)) >> 2;
                                g1[s] = ((raw1[s] + N - 1u) & (N - 1u)) >> 2;
                            } else {
                                uint32_t c0=(raw0[s]+N-44u)&(N-1u); uint32_t gg0=lora::utils::gray_encode(c0); g0[s]=((gg0+N-1u)&(N-1u))>>2;
                                uint32_t c1=(raw1[s]+N-44u)&(N-1u); uint32_t gg1=lora::utils::gray_encode(c1); g1[s]=((gg1+N-1u)&(N-1u))>>2;
                            }
                        }
                        uint8_t b0[5][8]{}, b1[5][8]{};
                        build_block_rows(g0, 0, b0);
                        build_block_rows(g1, 0, b1);
                        // Row-wise baseline
                        auto assemble_and_try = [&](const uint8_t L0[5][8], const uint8_t L1[5][8], const char* tag)->std::optional<lora::rx::LocalHeader> {
                            uint8_t cw[10]{};
                            for (uint32_t r=0;r<sf_app;++r){ uint16_t c=0; for(uint32_t i=0;i<8u;++i) c=(c<<1)|(L0[r][i]&1u); cw[r]=(uint8_t)(c&0xFF);} 
                            for (uint32_t r=0;r<sf_app;++r){ uint16_t c=0; for(uint32_t i=0;i<8u;++i) c=(c<<1)|(L1[r][i]&1u); cw[sf_app+r]=(uint8_t)(c&0xFF);} 
                            std::vector<uint8_t> nibb; nibb.reserve(10);
                            for (int k=0;k<10;++k){ auto dec=lora::utils::hamming_decode4(cw[k],8u,lora::utils::CodeRate::CR48,Th); if(!dec) return std::nullopt; nibb.push_back(dec->first & 0x0F);} 
                            for (int ord=0; ord<2; ++ord){ std::vector<uint8_t> hdr_try(5); for(int i=0;i<5;++i){ uint8_t n0=nibb[i*2], n1=nibb[i*2+1]; uint8_t lo=(ord==0)?n0:n1; uint8_t hi=(ord==0)?n1:n0; hdr_try[i]=(uint8_t)((hi<<4)|lo);} if(auto ok=parse_standard_lora_header(hdr_try.data(), hdr_try.size())) return ok; }
                            return std::nullopt;
                        };
                        if (auto ok = assemble_and_try(b0, b1, "base")) return ok;
                        // Small block1-only variants: row rotation, row/col reversal
                        for (uint32_t rot1 = 0; rot1 < sf_app; ++rot1) {
                            uint8_t t1[5][8]{}; for (uint32_t r=0;r<sf_app;++r){ uint32_t rr=(r+rot1)%sf_app; for (uint32_t c=0;c<8u;++c) t1[r][c]=b1[rr][c]; }
                            if (auto ok = assemble_and_try(b0, t1, "rot")) return ok;
                            uint8_t tr[5][8]{}; for (uint32_t r=0;r<sf_app;++r) for (uint32_t c=0;c<8u;++c) tr[r][c]=t1[sf_app-1u-r][c];
                            if (auto ok = assemble_and_try(b0, tr, "rot_rowrev")) return ok;
                            uint8_t t1c[5][8]{}; for (uint32_t r=0;r<sf_app;++r) for (uint32_t c=0;c<8u;++c) t1c[r][c]=t1[r][7u-c];
                            if (auto ok = assemble_and_try(b0, t1c, "rot_colrev")) return ok;
                        }
                    }
                }
            }
        }
    }

    return std::nullopt;
}

} // namespace lora::rx
