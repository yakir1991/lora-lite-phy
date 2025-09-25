#include "state_machine.hpp"
#include "dsp.hpp"
#include "lora/rx/lite/lora_header_decode.hpp"
#include "lora/rx/lite/lora_primitives.hpp"
#include "lora/rx/lite/lora_utils.hpp"
#include <cstdio>
#include <algorithm>
#include <limits>
#include <vector>

namespace lora::standalone {

Receiver::Receiver(const RxConfig& cfg) {
    ctx_.cfg = cfg;
    ctx_.N = 1u << cfg.sf;
    workspace_.init(cfg.sf);
    // Build reference chirps at OS=1 for demod after decimation
    auto refs = build_ref_chirps(cfg.sf, 1);
    up_ = std::move(refs.up);
    down_ = std::move(refs.down);
}

void Receiver::feed(std::span<const std::complex<float>> iq) {
    ctx_.buffer.insert(ctx_.buffer.end(), iq.begin(), iq.end());
}

std::vector<FrameOut> Receiver::run()
{
    const uint32_t N = ctx_.N;
    const size_t min_need = (ctx_.cfg.preamble_min + 8u) * static_cast<size_t>(N) * std::max(1u, ctx_.cfg.os);

    if (ctx_.buffer.size() < min_need) return {};

    workspace_.init(ctx_.cfg.sf);

    std::span<const std::complex<float>> samples(ctx_.buffer.data(), ctx_.buffer.size());

    std::vector<uint32_t> header_bins;
    std::vector<uint8_t> header_nibbles;
    auto header = lora::rx::gr::decode_header_with_preamble_cfo_sto_os(
        workspace_,
        samples,
        ctx_.cfg.sf,
        lora::rx::gr::CodeRate::CR45,
        ctx_.cfg.preamble_min,
        ctx_.cfg.sync_word,
        static_cast<int>(ctx_.cfg.os),
        &header_bins,
        &header_nibbles);

    if (!header) return {};

    ctx_.preamble_start_raw = workspace_.dbg_hdr_det_start_raw;
    ctx_.os = workspace_.dbg_hdr_os;
    ctx_.phase = workspace_.dbg_hdr_phase;

    FrameOut out;
    out.start_sample = workspace_.dbg_hdr_det_start_raw;
    out.os = workspace_.dbg_hdr_os;
    out.phase = workspace_.dbg_hdr_phase;
    out.cfo_fraction = workspace_.dbg_hdr_cfo;
    out.sfd_found = true;
    out.sfd_decim_pos = workspace_.dbg_hdr_sync_start;
    out.header_bins = header_bins;

    if (ctx_.cfg.debug_detection) {
        std::fprintf(stderr, "[header-bins] raw:");
        for (size_t i = 0; i < workspace_.dbg_hdr_syms_raw.size(); ++i)
            std::fprintf(stderr, " %u", workspace_.dbg_hdr_syms_raw[i]);
        std::fprintf(stderr, "\n[header-bins] corr:");
        for (size_t i = 0; i < workspace_.dbg_hdr_syms_corr.size(); ++i)
            std::fprintf(stderr, " %u", workspace_.dbg_hdr_syms_corr[i]);
        std::fprintf(stderr, "\n[header-bins] cfo_int=%d frac=%.6f\n", workspace_.dbg_hdr_cfo_int, workspace_.dbg_hdr_cfo);
    }

    out.header_bits.clear();
    out.header_bits.reserve(out.header_bins.size() * ctx_.cfg.sf);
    const uint32_t symbol_mask = (1u << ctx_.cfg.sf) - 1u;
    for (size_t idx = 0; idx < out.header_bins.size() && idx < workspace_.dbg_hdr_syms_corr.size(); ++idx) {
        uint32_t symbol = workspace_.dbg_hdr_syms_corr[idx] & symbol_mask;
        uint32_t decoded = lora::rx::gr::gray_decode(symbol);
        for (int bit = static_cast<int>(ctx_.cfg.sf) - 1; bit >= 0; --bit)
            out.header_bits.push_back((decoded >> bit) & 1u);
    }

    out.payload_len = header->payload_len;
    out.cr_idx = static_cast<int>(header->cr);
    out.has_crc = header->has_crc;
    out.header_crc_ok = true;
    if (ctx_.cfg.debug_detection) {
        std::fprintf(stderr,
                     "[header-debug] parsed len=%u cr=%u has_crc=%d nibs:",
                     static_cast<unsigned>(header->payload_len),
                     static_cast<unsigned>(header->cr),
                     header->has_crc ? 1 : 0);
        for (size_t idx = 0; idx < workspace_.dbg_hdr_nibbles_cr48.size(); ++idx) {
            std::fprintf(stderr, " %u", static_cast<unsigned>(workspace_.dbg_hdr_nibbles_cr48[idx] & 0x0Fu));
        }
        std::fprintf(stderr, "\n");
    }

    auto decim = lora::rx::gr::decimate_os_phase(samples, workspace_.dbg_hdr_os, workspace_.dbg_hdr_phase);
    if (decim.empty()) return {};

    const size_t header_start = workspace_.dbg_hdr_header_start;
    if (header_start >= decim.size()) return {};

    const uint32_t header_cr_plus4 = 8u;
    const size_t hdr_bytes = 5u;
    const size_t hdr_bits_exact = hdr_bytes * 2u * header_cr_plus4;
    const size_t block_bits = ctx_.cfg.sf * header_cr_plus4;
    const size_t hdr_bits_padded = ((hdr_bits_exact + block_bits - 1u) / block_bits) * block_bits;
    const size_t hdr_nsym = hdr_bits_padded / ctx_.cfg.sf;
    const size_t payload_start = header_start + hdr_nsym * N;
    if (payload_start > decim.size()) return {};

    auto decim_span = std::span<const std::complex<float>>(decim.data(), decim.size());
    int cfo_int = estimate_cfo_integer(decim_span, ctx_.cfg.sf, workspace_.dbg_hdr_start_decim, ctx_.cfg.preamble_min);
    out.cfo_integer = cfo_int;

    out.payload_bins.clear();
    out.payload_bytes.clear();
    out.payload_blocks_bits.clear();
    out.payload_bytes_raw.clear();
    out.payload_whitening_prns.clear();
    out.payload_crc_trace.clear();

    const int payload_len = static_cast<int>(header->payload_len);
    const bool has_crc = header->has_crc;
    const int cr_idx = static_cast<int>(header->cr);

    size_t i = payload_start;
    float eps = workspace_.dbg_hdr_cfo;

    if (cr_idx >= 1 && cr_idx <= 4) {
        const int cw_len = 4 + cr_idx;
        const uint32_t rows = ctx_.cfg.sf;
        const int expected_bytes = payload_len + (has_crc ? 2 : 0);
        const int expected_nibbles = expected_bytes * 2;
        auto gray_to_bin = [](uint32_t g) {
            uint32_t b = g;
            for (uint32_t shift = 1; shift < 32; shift <<= 1)
                b ^= (b >> shift);
            return b;
        };
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
                bool corrected = false;
                int dist = 0;
                uint8_t nib = hamming_payload_decode_bits_msb(bits8, cw_len, &corrected, &dist);
                uint8_t rev = static_cast<uint8_t>(((nib & 0x1) << 3) | ((nib & 0x2) << 1) | ((nib & 0x4) >> 1) | ((nib & 0x8) >> 3));
                nibbles_out.push_back(static_cast<uint8_t>(rev & 0x0F));
            }
        };

        std::vector<uint8_t> nibbles;
        if (expected_nibbles > 0) nibbles.reserve(static_cast<size_t>(expected_nibbles));
        size_t payload_symbol_index = 0;
        int col = 0;
        const uint32_t Nsym = 1u << ctx_.cfg.sf;

        while (expected_nibbles > 0 && static_cast<int>(nibbles.size()) < expected_nibbles && i + N <= decim.size()) {
            size_t span_len = (N + 4 <= decim.size() - i) ? (N + 4) : N;
            auto dr = demod_symbol_peak_fft_best_shift(
                std::span<const std::complex<float>>(decim.data() + i, span_len),
                std::span<const std::complex<float>>(down_.data(), N),
                eps,
                2);
            i += N;
            uint32_t bin = dr.bin;
            bin = (bin + N - static_cast<uint32_t>(((cfo_int % static_cast<int>(N)) + static_cast<int>(N)) % static_cast<int>(N))) % N;
            out.payload_bins.push_back(bin);
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

        if (expected_nibbles >= 0 && static_cast<int>(nibbles.size()) > expected_nibbles)
            nibbles.resize(expected_nibbles);

        if (!nibbles.empty()) {
            if (expected_bytes > 0) out.payload_bytes.reserve(expected_bytes);
            for (int k = 0; k + 1 < static_cast<int>(nibbles.size()); k += 2) {
                uint8_t low = static_cast<uint8_t>(nibbles[k] & 0x0F);
                uint8_t high = static_cast<uint8_t>(nibbles[k + 1] & 0x0F);
                out.payload_bytes.push_back(static_cast<uint8_t>((high << 4) | low));
            }
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
                if (((c & 0x8000) >> 8) ^ (newByte & 0x80)) c = static_cast<uint16_t>((c << 1) ^ 0x1021);
                else c = static_cast<uint16_t>(c << 1);
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
            bool is_payload_byte = j < payload_len;
            bool is_crc_byte = has_crc && j >= payload_len && j < payload_len + 2;
            uint16_t sem_before = crc_semtech;
            uint16_t gr_before = crc_gr;
            if (is_payload_byte) {
                uint8_t prn = lfsr.step();
                if (ctx_.cfg.trace_crc) out.payload_whitening_prns.push_back(prn);
                uint8_t dewhite = static_cast<uint8_t>(raw ^ prn);
                out.payload_bytes[j] = dewhite;
                crc_semtech = crc16_step(crc_semtech, dewhite);
                if (payload_len >= 2 && j < payload_len - 2) {
                    crc_gr = crc16_step(crc_gr, dewhite);
                } else if (payload_len >= 2) {
                    if (j == payload_len - 2) {
                        tail_second_last = dewhite;
                        have_tail_second = true;
                    }
                    if (j == payload_len - 1) {
                        tail_last = dewhite;
                        have_tail_last = true;
                    }
                }
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
                    entry.counted_gr = (payload_len >= 2 && j < payload_len - 2);
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
        if (payload_len >= 2 && have_tail_last && have_tail_second)
            crc_gr_val = static_cast<uint16_t>(crc_gr_val ^ tail_last ^ (static_cast<uint16_t>(tail_second_last) << 8));

        uint16_t crc_rx = 0;
        bool crc_rx_available = false;
        if (has_crc && static_cast<int>(out.payload_bytes.size()) >= payload_len + 2) {
            crc_rx = static_cast<uint16_t>(out.payload_bytes[payload_len] | (out.payload_bytes[payload_len + 1] << 8));
            crc_rx_available = true;
        }

        out.payload_crc_semtech = crc_semtech;
        out.payload_crc_gr = crc_gr_val;
        out.payload_crc_rx = crc_rx;
        if (has_crc) {
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

    size_t phase = workspace_.dbg_hdr_phase >= 0 ? static_cast<size_t>(workspace_.dbg_hdr_phase) : 0u;
    size_t raw_consumed = std::min(ctx_.buffer.size(), phase + static_cast<size_t>(workspace_.dbg_hdr_os) * i);
    ctx_.buffer.erase(ctx_.buffer.begin(), ctx_.buffer.begin() + static_cast<std::ptrdiff_t>(raw_consumed));
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
