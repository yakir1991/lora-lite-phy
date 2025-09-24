#include "state_machine.hpp"
#include "dsp.hpp"
#include <cstdio>
#include <algorithm>

namespace lora::standalone {

Receiver::Receiver(const RxConfig& cfg) {
    ctx_.cfg = cfg;
    ctx_.N = 1u << cfg.sf;
    // Build reference chirps at OS=1 for demod after decimation
    auto refs = build_ref_chirps(cfg.sf, 1);
    up_ = std::move(refs.up);
    down_ = std::move(refs.down);
}

void Receiver::feed(std::span<const std::complex<float>> iq) {
    ctx_.buffer.insert(ctx_.buffer.end(), iq.begin(), iq.end());
}

std::vector<FrameOut> Receiver::run() {
    const uint32_t N = ctx_.N;
    const size_t min_need = (ctx_.cfg.preamble_min + 8) * static_cast<size_t>(N) * std::max(1u, ctx_.cfg.os);

    // Simple blocking policy: if not enough samples even for preamble+some room, return
    if (ctx_.buffer.size() < min_need) return {};

    if (ctx_.state == RxState::SEARCH_PREAMBLE) {
        const int osc = static_cast<int>(ctx_.cfg.os);
        const int arr[3] = { osc, std::max(1, osc/2), 1 };
        auto det = detect_preamble_os(ctx_.buffer, ctx_.cfg.sf, ctx_.cfg.preamble_min, std::span<const int>(arr, 3));
        if (!det.found) return {};
        ctx_.preamble_start_raw = det.start_raw;
        ctx_.os = det.os; ctx_.phase = det.phase;
        if (ctx_.cfg.debug_detection) {
            std::fprintf(stderr, "[detect] preamble raw=%zu os=%d phase=%d\n",
                         ctx_.preamble_start_raw, ctx_.os, ctx_.phase);
        }
        ctx_.state = RxState::LOCATE_HEADER;
    }

    // Build decimated view from preamble start onward
    auto decim = decimate_os_phase(std::span<const std::complex<float>>(ctx_.buffer.data(), ctx_.buffer.size()), ctx_.os, ctx_.phase);
    size_t start_decim = ctx_.preamble_start_raw / static_cast<size_t>(ctx_.os);
    if (start_decim + (ctx_.cfg.preamble_min + 8) * N > decim.size()) return {};

    // Estimate fractional CFO from preamble and apply during demod
    float eps = estimate_cfo_epsilon(decim, ctx_.cfg.sf, start_decim, ctx_.cfg.preamble_min);

    // Locate SFD: after preamble_min upchirps, expect 2 downchirps. Use classifier over next ~6 symbols
    size_t scan_begin = start_decim + ctx_.cfg.preamble_min * N;
    size_t scan_end = std::min(decim.size(), scan_begin + 8 * static_cast<size_t>(N));
    size_t sfd_pos = scan_begin;
    bool sfd_found = false;
    // Build OS=1 references for classifier
    auto refs = build_ref_chirps(ctx_.cfg.sf, 1);
    for (size_t pos = scan_begin; pos + 2*N <= scan_end; pos += N) {
        auto s0 = classify_symbol(std::span<const std::complex<float>>(decim.data()+pos, N), std::span<const std::complex<float>>(refs.up.data(), N), std::span<const std::complex<float>>(refs.down.data(), N), eps);
        auto s1 = classify_symbol(std::span<const std::complex<float>>(decim.data()+pos+N, N), std::span<const std::complex<float>>(refs.up.data(), N), std::span<const std::complex<float>>(refs.down.data(), N), eps);
        // Heuristic: down_score >> up_score for downchirp
        if (s0.down_score > 4.0f * s0.up_score && s1.down_score > 4.0f * s1.up_score) {
            sfd_found = true;
            sfd_pos = pos;
            break;
        }
    }
    // Header starts after SFD (2 symbols)
    size_t hdr_start = (sfd_found ? (sfd_pos + 2*N) : (start_decim + (ctx_.cfg.preamble_min + 4) * N));

    // Estimate integer CFO (# bins) and use it to rotate detected bins back
    int cfo_int = estimate_cfo_integer(decim, ctx_.cfg.sf, start_decim, ctx_.cfg.preamble_min);
    FrameOut out;
    out.start_sample = ctx_.preamble_start_raw;
    out.os = ctx_.os;
    out.phase = ctx_.phase;
    out.cfo_fraction = eps;
    out.cfo_integer = cfo_int;
    out.sfd_found = sfd_found;
    out.sfd_decim_pos = sfd_pos;
    if (ctx_.cfg.debug_detection) {
        std::fprintf(stderr, "[detect] frac CFO=%.6f bins int CFO=%d\n", eps, cfo_int);
        if (sfd_found) {
            std::fprintf(stderr, "[detect] SFD at decim=%zu (hdr start=%zu)\n", sfd_pos, hdr_start);
        } else {
            std::fprintf(stderr, "[detect] SFD fallback hdr start=%zu\n", hdr_start);
        }
    }

    // Helper lambda to demod+decode header from a given decim position
    const uint32_t sf = ctx_.cfg.sf;
    const uint32_t sf_app = (sf > 2u) ? (sf - 2u) : sf;
    const uint32_t cw_len = 8u; // CR4/8 for header
    auto gray_to_bin = [](uint32_t g){ uint32_t b = g; for (int s=1; s<16; ++s) b ^= (g>>s); return b; };
    struct HdrTry {
        std::vector<uint32_t> bins;
        std::vector<uint8_t> nibbles;
        std::vector<uint8_t> inter_bits;
        std::vector<uint8_t> deinter_bits;
        int len{-1};
        int cr{-1};
        bool has_crc{false};
        bool crc_ok{false};
        int dist{1000000000};
    };
    auto try_decode = [&](size_t pos)->HdrTry{
        HdrTry ht;
        // Demod 8 symbols
        for (int s = 0; s < 8 && pos + N <= decim.size(); ++s, pos += N) {
            auto dr = demod_symbol_peak_fft_best_shift(std::span<const std::complex<float>>(decim.data()+pos, N+4 <= decim.size()-pos ? N+4 : N), std::span<const std::complex<float>>(down_.data(), N), eps, 2);
            uint32_t bin = dr.bin;
            bin = (bin + N - (static_cast<uint32_t>( ( (cfo_int % static_cast<int>(N)) + static_cast<int>(N) ) % static_cast<int>(N) ))) % N;
            ht.bins.push_back(bin);
        }
        // Build inter matrix
        std::vector<uint8_t> inter(static_cast<size_t>(sf_app) * cw_len);
        for (uint32_t col = 0; col < ht.bins.size() && col < cw_len; ++col) {
            uint32_t b_bin = gray_to_bin(ht.bins[col]);
            uint32_t Nsym = (1u << sf);
            uint32_t s_adj = (b_bin + Nsym - 1u) % Nsym;
            uint32_t s_hdr = s_adj >> 2;
            for (uint32_t row = 0; row < sf_app; ++row) {
                uint32_t bit = (s_hdr >> (sf_app - 1u - row)) & 1u;
                inter[col * sf_app + row] = static_cast<uint8_t>(bit);
            }
        }
        // Deinterleave
        std::vector<uint8_t> deinter(static_cast<size_t>(sf_app) * cw_len);
        for (uint32_t col = 0; col < cw_len; ++col) {
            for (uint32_t row = 0; row < sf_app; ++row) {
                int dest_row = static_cast<int>(col) - static_cast<int>(row) - 1;
                dest_row %= static_cast<int>(sf_app);
                if (dest_row < 0) dest_row += static_cast<int>(sf_app);
                deinter[static_cast<size_t>(dest_row) * cw_len + col] = inter[col * sf_app + row];
            }
        }
        if (ctx_.cfg.capture_header_bits) {
            ht.inter_bits = inter;
            ht.deinter_bits = deinter;
        }
        // Hamming(8,4)
        ht.nibbles.reserve(sf_app);
        int total_dist = 0;
        for (uint32_t r = 0; r < sf_app; ++r) {
            uint8_t bits8[8]{};
            for (uint32_t c = 0; c < cw_len; ++c) bits8[c] = deinter[r * cw_len + c];
            bool corrected=false; int dist=0;
            uint8_t nib = hamming84_decode_bits_msb(bits8, &corrected, &dist);
            total_dist += dist;
            // Reverse bit order in nibble to match GR output (MSB<=>LSB)
            uint8_t rev = static_cast<uint8_t>(((nib & 0x1) << 3) | ((nib & 0x2) << 1) | ((nib & 0x4) >> 1) | ((nib & 0x8) >> 3));
            ht.nibbles.push_back(rev & 0x0F);
        }
        ht.dist = total_dist;
        if (ht.nibbles.size() >= 5) {
            // Try all row rotations to account for potential row index offset
            int best_err=1e9; int best_rot=0; int best_len=-1; int best_cr=-1; bool best_has_crc=false; bool best_crc_ok=false;
            for (uint32_t rot = 0; rot < sf_app; ++rot) {
                auto nib = ht.nibbles; // copy
                // rotate left by rot
                std::rotate(nib.begin(), nib.begin() + (rot % nib.size()), nib.end());
                int len_byte = ((nib[0] & 0x0F) << 4) | (nib[1] & 0x0F);
                uint8_t flags = nib[2] & 0x0F;
                bool has_crc = (flags & 0x1) != 0;
                int cr = (flags >> 1) & 0x7;
                uint8_t hdr_chk = static_cast<uint8_t>(((nib[3] & 0x1) << 4) | (nib[4] & 0x0F));
                uint8_t n0 = nib[0] & 0x0F;
                uint8_t n1 = nib[1] & 0x0F;
                uint8_t n2 = nib[2] & 0x0F;
                bool c4 = ((n0 & 0x8) >> 3) ^ ((n0 & 0x4) >> 2) ^ ((n0 & 0x2) >> 1) ^ (n0 & 0x1);
                bool c3 = ((n0 & 0x8) >> 3) ^ ((n1 & 0x8) >> 3) ^ ((n1 & 0x4) >> 2) ^ ((n1 & 0x2) >> 1) ^ (n2 & 0x1);
                bool c2 = ((n0 & 0x4) >> 2) ^ ((n1 & 0x8) >> 3) ^ (n1 & 0x1) ^ ((n2 & 0x8) >> 3) ^ ((n2 & 0x2) >> 1);
                bool c1 = ((n0 & 0x2) >> 1) ^ ((n1 & 0x4) >> 2) ^ (n1 & 0x1) ^ ((n2 & 0x4) >> 2) ^ ((n2 & 0x2) >> 1) ^ (n2 & 0x1);
                bool c0 = (n0 & 0x1) ^ ((n1 & 0x2) >> 1) ^ ((n2 & 0x8) >> 3) ^ ((n2 & 0x4) >> 2) ^ ((n2 & 0x2) >> 1) ^ (n2 & 0x1);
                uint8_t calc = static_cast<uint8_t>((c4 << 4) | (c3 << 3) | (c2 << 2) | (c1 << 1) | c0);
                int err = static_cast<int>(hdr_chk) - static_cast<int>(calc);
                bool ok = (hdr_chk == calc) && (len_byte > 0);
                if (ok) { best_rot = static_cast<int>(rot); best_len = len_byte; best_cr = cr; best_has_crc = has_crc; best_crc_ok = true; break; }
                int adiff = std::abs(err);
                if (adiff < best_err) { best_err = adiff; best_rot = static_cast<int>(rot); best_len = len_byte; best_cr = cr; best_has_crc = has_crc; }
            }
            if (best_rot != 0) {
                std::rotate(ht.nibbles.begin(), ht.nibbles.begin() + (best_rot % ht.nibbles.size()), ht.nibbles.end());
            }
            ht.len = best_len; ht.cr = best_cr; ht.has_crc = best_has_crc; ht.crc_ok = best_crc_ok;
        }
        return ht;
    };

    // Search small offsets around hdr_start
    HdrTry best{}; bool any=false; int best_score = 1e9; size_t best_pos = hdr_start;
    for (int off_sym = -1; off_sym <= 1; ++off_sym) {
        if (off_sym < 0 && hdr_start < static_cast<size_t>(-off_sym)*N) continue;
        size_t pos = hdr_start + static_cast<int64_t>(off_sym) * static_cast<int64_t>(N);
        if (pos + 8*N > decim.size()) continue;
        auto ht = try_decode(pos);
        int score = ht.crc_ok ? 0 : (ht.dist + 10); // prefer checksum valid; otherwise use distance
        if (!any || score < best_score) { best = std::move(ht); best_score = score; best_pos = pos; any = true; }
        if (ht.crc_ok) break; // good enough
    }

    out.header_bins = best.bins;
    // Expose header bits (from selected bins)
    for (auto bin : best.bins) {
        uint32_t b = gray_to_bin(bin);
        for (int k = static_cast<int>(sf) - 1; k >= 0; --k)
            out.header_bits.push_back( (b >> k) & 1u );
    }
    if (ctx_.cfg.capture_header_bits) {
        out.header_rows = sf_app;
        out.header_cols = cw_len;
        out.header_interleaver_bits = best.inter_bits;
        out.header_deinterleaver_bits = best.deinter_bits;
    }
    // Store parsed header fields
    out.payload_len = best.len;
    out.cr_idx = best.cr;
    out.has_crc = best.has_crc;
    out.header_crc_ok = best.crc_ok;
    // Advance i based on best alignment used
    size_t i = best_pos + 8*N;

    // Payload decode path
    if (best.cr >= 1 && best.cr <= 4 && best.len >= 0) {
        const int cr_app = best.cr;
        const int cw_len = 4 + cr_app;
        const uint32_t rows = ctx_.cfg.sf;
        const int expected_bytes = std::max(best.len, 0) + (best.has_crc ? 2 : 0);
        const int expected_nibbles = expected_bytes * 2;
        auto gray_to_bin = [](uint32_t g){ uint32_t b = g; for (int s=1; s<16; ++s) b ^= (g>>s); return b; };
        std::vector<uint8_t> inter(static_cast<size_t>(rows) * cw_len, 0u);

        auto push_decode_block = [&](std::vector<uint8_t>& nibbles_out, size_t symbol_offset) {
            std::vector<uint8_t> deinter(static_cast<size_t>(rows) * cw_len);
            for (int col = 0; col < cw_len; ++col) {
                for (uint32_t row = 0; row < rows; ++row) {
                    int dest_row = ((col - static_cast<int>(row) - 1) % static_cast<int>(rows) + static_cast<int>(rows)) % static_cast<int>(rows);
                    deinter[static_cast<size_t>(dest_row) * cw_len + col] = inter[static_cast<size_t>(col) * rows + row];
                }
            }
            if (ctx_.cfg.capture_payload_blocks) {
                FrameOut::PayloadBlockBits block;
                block.rows = rows;
                block.cols = static_cast<uint32_t>(cw_len);
                block.symbol_offset = symbol_offset;
                block.inter_bits = inter;
                block.deinter_bits = deinter;
                out.payload_blocks_bits.push_back(std::move(block));
            }
            for (uint32_t r = 0; r < rows; ++r) {
                uint8_t bits8[8]{};
                for (int c = 0; c < cw_len; ++c) bits8[c] = deinter[r * cw_len + c] & 1u;
                bool corr = false; int dist = 0;
                uint8_t nib = hamming_payload_decode_bits_msb(bits8, cw_len, &corr, &dist);
                uint8_t rev = static_cast<uint8_t>(((nib & 0x1) << 3) | ((nib & 0x2) << 1) | ((nib & 0x4) >> 1) | ((nib & 0x8) >> 3));
                nibbles_out.push_back(static_cast<uint8_t>(rev & 0x0F));
            }
        };

        std::vector<uint8_t> nibbles;
        if (expected_nibbles > 0) nibbles.reserve(static_cast<size_t>(expected_nibbles));
        size_t payload_symbol_index = 0;
        int col = 0;
        while (expected_nibbles > 0 && static_cast<int>(nibbles.size()) < expected_nibbles && i + N <= decim.size()) {
            auto dr = demod_symbol_peak_fft_best_shift(std::span<const std::complex<float>>(decim.data()+i, N+4 <= decim.size()-i ? N+4 : N), std::span<const std::complex<float>>(down_.data(), N), eps, 2);
            i += N;
            uint32_t bin = dr.bin;
            bin = (bin + N - (static_cast<uint32_t>(((cfo_int % static_cast<int>(N)) + static_cast<int>(N)) % static_cast<int>(N)))) % N;
            out.payload_bins.push_back(bin);
            uint32_t Nsym = (1u << ctx_.cfg.sf);
            uint32_t b_bin = gray_to_bin(bin);
            uint32_t s_adj = (b_bin + Nsym - 1u) % Nsym;
            for (uint32_t row = 0; row < rows; ++row) {
                uint32_t bit = (s_adj >> (ctx_.cfg.sf - 1u - row)) & 1u;
                inter[static_cast<size_t>(col) * rows + row] = static_cast<uint8_t>(bit);
            }
            ++col;
            ++payload_symbol_index;
            if (col == cw_len) {
                push_decode_block(nibbles, payload_symbol_index - static_cast<size_t>(cw_len));
                col = 0;
                std::fill(inter.begin(), inter.end(), 0u);
            }
        }
        if (expected_nibbles >= 0 && static_cast<int>(nibbles.size()) > expected_nibbles) nibbles.resize(expected_nibbles);

        out.payload_bytes.clear();
        if (expected_bytes > 0) out.payload_bytes.reserve(expected_bytes);
        for (int k = 0; k + 1 < static_cast<int>(nibbles.size()); k += 2) {
            uint8_t low = static_cast<uint8_t>(nibbles[k] & 0x0F);
            uint8_t high = static_cast<uint8_t>(nibbles[k + 1] & 0x0F);
            out.payload_bytes.push_back(static_cast<uint8_t>((high << 4) | low));
        }

        if (ctx_.cfg.trace_crc) {
            out.payload_bytes_raw = out.payload_bytes;
        } else {
            out.payload_bytes_raw.clear();
        }
        out.payload_whitening_prns.clear();
        out.payload_crc_trace.clear();

        struct WhiteningLfsr {
            uint8_t state{0xFF};
            uint8_t step() {
                uint8_t prn = state;
                uint8_t b3 = static_cast<uint8_t>((state >> 3) & 1u);
                uint8_t b4 = static_cast<uint8_t>((state >> 4) & 1u);
                uint8_t b5 = static_cast<uint8_t>((state >> 5) & 1u);
                uint8_t b7 = static_cast<uint8_t>((state >> 7) & 1u);
                uint8_t next = static_cast<uint8_t>(b7 ^ b5 ^ b4 ^ b3);
                state = static_cast<uint8_t>(((state << 1) | next) & 0xFFu);
                return prn;
            }
        };
        auto crc16_step = [](uint16_t crc, uint8_t byte) {
            uint16_t c = crc;
            uint8_t newByte = byte;
            for (unsigned char bit = 0; bit < 8; ++bit) {
                if (((c & 0x8000) >> 8) ^ (newByte & 0x80)) {
                    c = static_cast<uint16_t>((c << 1) ^ 0x1021);
                } else {
                    c = static_cast<uint16_t>(c << 1);
                }
                newByte <<= 1;
            }
            return c;
        };

        WhiteningLfsr lfsr;
        uint16_t crc_semtech = 0x0000;
        uint16_t crc_gr = 0x0000;
        uint8_t tail_last = 0u;
        uint8_t tail_second_last = 0u;
        bool have_tail_last = false;
        bool have_tail_second = false;

        for (int j = 0; j < static_cast<int>(out.payload_bytes.size()); ++j) {
            uint8_t raw = out.payload_bytes[j];
            bool is_payload_byte = j < best.len;
            bool is_crc_byte = best.has_crc && j >= best.len && j < best.len + 2;
            uint16_t sem_before = crc_semtech;
            uint16_t gr_before = crc_gr;
            if (is_payload_byte) {
                uint8_t prn = lfsr.step();
                if (ctx_.cfg.trace_crc) out.payload_whitening_prns.push_back(prn);
                uint8_t dewhite = static_cast<uint8_t>(raw ^ prn);
                out.payload_bytes[j] = dewhite;
                crc_semtech = crc16_step(crc_semtech, dewhite);
                if (best.len >= 2 && j < best.len - 2) {
                    crc_gr = crc16_step(crc_gr, dewhite);
                } else {
                    if (best.len >= 2) {
                        if (j == best.len - 2) {
                            tail_second_last = dewhite;
                            have_tail_second = true;
                        }
                        if (j == best.len - 1) {
                            tail_last = dewhite;
                            have_tail_last = true;
                        }
                    }
                }
            } else {
            }
            if (ctx_.cfg.trace_crc) {
                FrameOut::PayloadTraceEntry entry;
                entry.index = j;
                entry.raw_byte = raw;
                entry.is_crc_byte = is_crc_byte;
                entry.crc_semtech_before = sem_before;
                entry.crc_gr_before = gr_before;
                entry.crc_semtech_after = crc_semtech;
                entry.crc_gr_after = crc_gr;
                if (is_payload_byte) {
                    entry.whitening = out.payload_whitening_prns.empty() ? 0 : out.payload_whitening_prns.back();
                    entry.dewhitened = out.payload_bytes[j];
                    entry.counted_semtech = true;
                    entry.counted_gr = (best.len >= 2 && j < best.len - 2);
                } else {
                    entry.whitening = 0;
                    entry.dewhitened = raw;
                    entry.counted_semtech = false;
                    entry.counted_gr = false;
                }
                out.payload_crc_trace.push_back(entry);
            }
        }

        uint16_t crc_gr_val = crc_gr;
        if (best.len >= 2 && have_tail_last && have_tail_second) {
            crc_gr_val = static_cast<uint16_t>(crc_gr_val ^ tail_last ^ (static_cast<uint16_t>(tail_second_last) << 8));
        }

        uint16_t crc_rx = 0;
        bool crc_rx_available = false;
        if (best.has_crc && static_cast<int>(out.payload_bytes.size()) >= best.len + 2) {
            crc_rx = static_cast<uint16_t>(out.payload_bytes[best.len] | (out.payload_bytes[best.len + 1] << 8));
            crc_rx_available = true;
        }

        out.payload_crc_semtech = crc_semtech;
        out.payload_crc_gr = crc_gr_val;
        out.payload_crc_rx = crc_rx;
        if (best.has_crc) {
            if (crc_rx_available) {
                out.payload_crc_semtech_ok = (crc_semtech == crc_rx);
                out.payload_crc_gr_ok = (crc_gr_val == crc_rx);
                out.payload_crc_ok = out.payload_crc_semtech_ok;
            } else {
                out.payload_crc_semtech_ok = false;
                out.payload_crc_gr_ok = false;
                out.payload_crc_ok = false;
            }
        } else {
            out.payload_crc_semtech_ok = true;
            out.payload_crc_gr_ok = true;
            out.payload_crc_ok = true;
        }
    }

    // Advance buffer to after what we consumed + small history
    size_t consume_raw = (i + N <= decim.size()) ? (i * static_cast<size_t>(ctx_.os)) : ctx_.buffer.size();
    if (consume_raw > ctx_.buffer.size()) consume_raw = ctx_.buffer.size();
    ctx_.buffer.erase(ctx_.buffer.begin(), ctx_.buffer.begin() + static_cast<std::ptrdiff_t>(consume_raw));
    ctx_.state = RxState::SEARCH_PREAMBLE;
    ready_.push_back(std::move(out));
    auto res = std::move(ready_);
    ready_.clear();
    return res;
}

std::optional<FrameOut> Receiver::process(std::span<const std::complex<float>> iq)
{
    feed(iq);
    auto frames = run();
    if (frames.empty()) return std::nullopt;
    return frames.front();
}

} // namespace lora::standalone
