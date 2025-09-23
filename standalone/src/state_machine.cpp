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

    // Helper lambda to demod+decode header from a given decim position
    const uint32_t sf = ctx_.cfg.sf;
    const uint32_t sf_app = (sf > 2u) ? (sf - 2u) : sf;
    const uint32_t cw_len = 8u; // CR4/8 for header
    auto gray_to_bin = [](uint32_t g){ uint32_t b = g; for (int s=1; s<16; ++s) b ^= (g>>s); return b; };
    struct HdrTry { std::vector<uint32_t> bins; std::vector<uint8_t> nibbles; int len{-1}; int cr{-1}; bool has_crc{false}; bool crc_ok{false}; int dist{1000000000}; };
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
            bool any_ok=false; int best_err=1e9; int best_rot=0; int best_len=-1; int best_cr=-1; bool best_has_crc=false; bool best_crc_ok=false;
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
                if (ok) { best_rot = static_cast<int>(rot); best_len = len_byte; best_cr = cr; best_has_crc = has_crc; best_crc_ok = true; any_ok=true; break; }
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
    // Store parsed header fields
    out.payload_len = best.len;
    out.cr_idx = best.cr;
    out.has_crc = best.has_crc;
    out.header_crc_ok = best.crc_ok;
    // Advance i based on best alignment used
    size_t i = best_pos + 8*N;

    // Payload decode path
    if (best.cr >= 1 && best.cr <= 4 && best.len > 0) {
        const int cr_app = best.cr; // coding rate index maps to CR=4/(4+cr)
        const int cw_len = 4 + cr_app; // bits per codeword row
    const uint32_t rows = ctx_.cfg.sf; // rows in interleaver for payload (LDRO not handled here)
        // Number of codeword columns = number of payload symbols to cover required bytes (nibbles)
        // Each column contributes one bit to each row; 1 symbol -> rows bits -> after deinterleaving produce rows codewords of cw_len.
        // We need 2*len bytes including potential CRC later, but we don't know CRC yet; demod until we at least fill payload length + optional CRC bytes
        const int expected_bytes = best.len + (best.has_crc ? 2 : 0);
        const int expected_nibbles = expected_bytes * 2;
        // Each deinterleaved pass over rows produces rows nibbles (one per row). We need expected_nibbles total.
        // For each block of cw_len symbols we can deinterleave into rows codewords of cw_len bits; but in LoRa, interleaver length is cw_len columns per codeword group.
        // We iterate symbol by symbol, filling inter[col][row] = bit, then after cw_len columns, deinterleave rows and Hamming-decode to 1 nibble per row.
    std::vector<uint8_t> inter(static_cast<size_t>(rows) * cw_len);
        auto gray_to_bin = [](uint32_t g){ uint32_t b = g; for (int s=1; s<16; ++s) b ^= (g>>s); return b; };
    auto push_decode_block = [&](std::vector<uint8_t>& nibbles_out){
            // Deinterleave diagonally: dest_row = mod(col - row - 1, rows)
            std::vector<uint8_t> deinter(static_cast<size_t>(rows) * cw_len);
            for (int col = 0; col < cw_len; ++col) {
                for (uint32_t row = 0; row < rows; ++row) {
                    int dest_row = ( (col - static_cast<int>(row) - 1) % static_cast<int>(rows) + static_cast<int>(rows) ) % static_cast<int>(rows);
                    deinter[static_cast<size_t>(dest_row) * cw_len + col] = inter[static_cast<size_t>(col) * rows + row];
                }
            }
            // For each row, take cw_len bits MSB-first and Hamming-decode to nibble
            for (uint32_t r = 0; r < rows; ++r) {
                uint8_t bits8[8]{};
                for (int c = 0; c < cw_len; ++c) bits8[c] = deinter[r * cw_len + c] & 1u;
                bool corr=false; int dist=0;
                uint8_t nib = hamming_payload_decode_bits_msb(bits8, cw_len, &corr, &dist);
                // Reverse bit order in nibble to match GR implementation (MSB<=>LSB)
                uint8_t rev = static_cast<uint8_t>(((nib & 0x1) << 3) | ((nib & 0x2) << 1) | ((nib & 0x4) >> 1) | ((nib & 0x8) >> 3));
                nibbles_out.push_back(rev & 0x0F);
            }
        };

        std::vector<uint8_t> nibbles;
        nibbles.reserve(static_cast<size_t>(expected_nibbles));
        int col = 0;
        while (static_cast<int>(nibbles.size()) < expected_nibbles && i + N <= decim.size()) {
            // Demod one symbol
            auto dr = demod_symbol_peak_fft_best_shift(std::span<const std::complex<float>>(decim.data()+i, N+4 <= decim.size()-i ? N+4 : N), std::span<const std::complex<float>>(down_.data(), N), eps, 2);
            i += N;
            uint32_t bin = dr.bin;
            bin = (bin + N - (static_cast<uint32_t>( ( (cfo_int % static_cast<int>(N)) + static_cast<int>(N) ) % static_cast<int>(N) ))) % N;
            out.payload_bins.push_back(bin);
            // Map to Gray->bin, header reduced-rate does not apply; take msb-first sf bits
            uint32_t Nsym = (1u << ctx_.cfg.sf);
            uint32_t b_bin = gray_to_bin(bin);
            // Apply LoRa symbol alignment: shift by -1 after Gray decode, like header
            uint32_t s_adj = (b_bin + Nsym - 1u) % Nsym;
            for (uint32_t row = 0; row < rows; ++row) {
                uint32_t bit = (s_adj >> (ctx_.cfg.sf - 1u - row)) & 1u;
                inter[static_cast<size_t>(col) * rows + row] = static_cast<uint8_t>(bit);
            }
            col++;
            if (col == cw_len) {
                push_decode_block(nibbles);
                col = 0;
            }
        }
        // Truncate to requested nibbles
        if (static_cast<int>(nibbles.size()) > expected_nibbles) nibbles.resize(expected_nibbles);
        // Assemble bytes
        out.payload_bytes.clear(); out.payload_bytes.reserve(expected_bytes);
        for (int k = 0; k + 1 < expected_nibbles; k += 2) {
            uint8_t low = static_cast<uint8_t>(nibbles[k] & 0x0F);
            uint8_t high = static_cast<uint8_t>(nibbles[k+1] & 0x0F);
            uint8_t byte = static_cast<uint8_t>((high << 4) | low);
            out.payload_bytes.push_back(byte);
        }
        // Dewhitening: XOR only first payload_len bytes with whitening sequence; leave CRC bytes unchanged
        static const uint8_t whitening_seq[] = {
            0xFF,0xFE,0xFC,0xF8,0xF0,0xE1,0xC2,0x85,0x0B,0x17,0x2F,0x5E,0xBC,0x78,0xF1,0xE3,
            0xC6,0x8D,0x1A,0x34,0x68,0xD0,0xA0,0x40,0x80,0x01,0x02,0x04,0x08,0x11,0x23,0x47,
            0x8E,0x1C,0x38,0x71,0xE2,0xC4,0x89,0x12,0x25,0x4B,0x97,0x2E,0x5C,0xB8,0x70,0xE0,
            0xC0,0x81,0x03,0x06,0x0C,0x19,0x32,0x64,0xC9,0x92,0x24,0x49,0x93,0x26,0x4D,0x9B,
            0x37,0x6E,0xDC,0xB9,0x72,0xE4,0xC8,0x90,0x20,0x41,0x82,0x05,0x0A,0x15,0x2B,0x56,
            0xAD,0x5B,0xB6,0x6D,0xDA,0xB5,0x6B,0xD6,0xAC,0x59,0xB2,0x65,0xCB,0x96,0x2C,0x58,
            0xB0,0x61,0xC3,0x87,0x0F,0x1F,0x3E,0x7D,0xFB,0xF6,0xED,0xDB,0xB7,0x6F,0xDE,0xBD,
            0x7A,0xF5,0xEB,0xD7,0xAE,0x5D,0xBA,0x74,0xE8,0xD1,0xA2,0x44,0x88,0x10,0x21,0x43,
            0x86,0x0D,0x1B,0x36,0x6C,0xD8,0xB1,0x63,0xC7,0x8F,0x1E,0x3C,0x79,0xF3,0xE7,0xCE,
            0x9C,0x39,0x73,0xE6,0xCC,0x98,0x31,0x62,0xC5,0x8B,0x16,0x2D,0x5A,0xB4,0x69,0xD2,
            0xA4,0x48,0x91,0x22,0x45,0x8A,0x14,0x29,0x52,0xA5,0x4A,0x95,0x2A,0x54,0xA9,0x53,
            0xA7,0x4E,0x9D,0x3B,0x77,0xEE,0xDD,0xBB,0x76,0xEC,0xD9,0xB3,0x67,0xCF,0x9E,0x3D,
            0x7B,0xF7,0xEF,0xDF,0xBF,0x7E,0xFD,0xFA,0xF4,0xE9,0xD3,0xA6,0x4C,0x99,0x33,0x66,
            0xCD,0x9A,0x35,0x6A,0xD4,0xA8,0x51,0xA3,0x46,0x8C,0x18,0x30,0x60,0xC1,0x83,0x07,
            0x0E,0x1D,0x3A,0x75,0xEA,0xD5,0xAA,0x55,0xAB,0x57,0xAF,0x5F,0xBE,0x7C,0xF9,0xF2,
            0xE5,0xCA,0x94,0x28,0x50,0xA1,0x42,0x84,0x09,0x13,0x27,0x4F,0x9F,0x3F,0x7F
        };
        for (int j = 0; j < best.len && j < static_cast<int>(out.payload_bytes.size()); ++j) {
            out.payload_bytes[j] = static_cast<uint8_t>(out.payload_bytes[j] ^ whitening_seq[j]);
        }
        // CRC16 (poly 0x1021) per gr-lora-sdr, computed over dewhitened payload bytes (length = payload_len),
        // and compared directly to the two following CRC bytes (LSB then MSB). CRC bytes are not dewhitened.
        if (best.has_crc && static_cast<int>(out.payload_bytes.size()) >= best.len + 2) {
            auto crc16 = [](const uint8_t* data, uint32_t len){
                uint16_t crc = 0x0000;
                for (uint32_t i2 = 0; i2 < len; ++i2) {
                    uint8_t newByte = data[i2];
                    for (unsigned char j = 0; j < 8; ++j) {
                        if (((crc & 0x8000) >> 8) ^ (newByte & 0x80)) crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
                        else crc = static_cast<uint16_t>(crc << 1);
                        newByte <<= 1;
                    }
                }
                return crc;
            };
            uint16_t crc_calc = crc16(out.payload_bytes.data(), static_cast<uint32_t>(best.len));
            uint16_t crc_rx = static_cast<uint16_t>(out.payload_bytes[best.len] | (out.payload_bytes[best.len + 1] << 8));
            out.payload_crc_ok = (crc_calc == crc_rx);
        } else if (!best.has_crc && static_cast<int>(out.payload_bytes.size()) >= best.len) {
            out.payload_crc_ok = true; // no CRC to check
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
