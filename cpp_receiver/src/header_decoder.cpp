#include "header_decoder.hpp"

#include "chirp_generator.hpp"
#include "hamming.hpp"
#include "fft_utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>
#include <vector>
#include <cstdint>
#include <numeric>
#include <cstdlib>

// LoRa's explicit header encodes payload metadata across eight symbols with a
// combination of Gray mapping, interleaving, and Hamming-like parity. The goal of
// this file is to keep that dance understandable: we demodulate tones, undo the
// mapping step by step, then validate the CRC5 before surfacing fields back to the
// caller. Inline helpers focus on the math (FFT, Gray tables, parity checks) so
// the top-level `HeaderDecoder::decode` reads like a sequential recipe.

namespace lora {

namespace {

// Small front-end rise-time compensation used to align the header start similar to the reference.
constexpr double kTriseSeconds = 50e-6;

using CDouble = std::complex<double>;
using CFloat = std::complex<float>;

// Cast input sample typedef to complex<double> for numerics.
CDouble to_cdouble(const HeaderDecoder::Sample &sample) {
    return CDouble(static_cast<double>(sample.real()), static_cast<double>(sample.imag()));
}

// FFT utility using lora::fft with optional inverse sign; zero-pads to fft_len.
template <typename Complex>
std::vector<CFloat> compute_spectrum_fft(const std::vector<Complex> &input,
                                         std::size_t fft_len,
                                         bool inverse,
                                         lora::fft::Scratch &fft_scratch) {
    std::vector<CFloat> spectrum(fft_len, CFloat{0.0f, 0.0f});
    const std::size_t limit = std::min<std::size_t>(fft_len, input.size());
    for (std::size_t i = 0; i < limit; ++i) {
        spectrum[i] = CFloat(static_cast<float>(input[i].real()),
                             static_cast<float>(input[i].imag()));
    }
    lora::fft::transform_pow2(spectrum, inverse, fft_scratch);
    return spectrum;
}

// Argmax by magnitude helper for spectral peaks.
template <typename Complex>
std::size_t argmax_abs(const std::vector<Complex> &vec) {
    std::size_t idx = 0;
    double max_mag = 0.0;
    for (std::size_t i = 0; i < vec.size(); ++i) {
        const double mag = std::abs(vec[i]);
        if (mag > max_mag) {
            max_mag = mag;
            idx = i;
        }
    }
    return idx;
}

} // namespace

namespace {

int positive_mod(int value, int modulus) {
    const int mod = value % modulus;
    return (mod < 0) ? mod + modulus : mod;
}

std::vector<int> bits_from_uint(int value, int width) {
    std::vector<int> bits(width, 0);
    for (int i = 0; i < width; ++i) {
        const int shift = width - 1 - i;
        bits[i] = (value >> shift) & 1;
    }
    return bits;
}

int bits_to_uint(const std::vector<int> &bits) {
    int value = 0;
    for (int bit : bits) {
        value = (value << 1) | (bit & 1);
    }
    return value;
}

struct HeaderPipelineState {
    int sf = 0;
    int ppm = 0;
    int cw_cols = 0;
    std::vector<int> column_bits;                       // column-major Gray-decoded bits
    std::vector<std::vector<int>> interleaved_columns;  // S matrix (columns=8, rows=ppm)
    std::vector<std::vector<int>> rotated_columns;      // C matrix after circular shifts
};

std::optional<HeaderPipelineState> build_header_pipeline(int sf,
                                                         std::size_t K,
                                                         const std::array<int, 8> &raw_symbols) {
    const int ppm = std::max(1, sf - 2);
    const int n_sym_hdr = 8;
    const int cw_cols = 8; // header-specific: 4 data + 4 parity bits per column

    HeaderPipelineState state;
    state.sf = sf;
    state.ppm = ppm;
    state.cw_cols = cw_cols;
    state.column_bits.assign(ppm * n_sym_hdr, 0);
    state.interleaved_columns.assign(n_sym_hdr, std::vector<int>(ppm, 0));
    state.rotated_columns.assign(ppm, std::vector<int>(cw_cols, 0));

    const int wrapK = static_cast<int>(K);
    const int header_de_shift = (sf > 6) ? 2 : 0;
    const int value_mask = (sf >= 0 && sf < 31) ? ((1 << sf) - 1) : -1;
    for (int sym = 0; sym < n_sym_hdr; ++sym) {
        int k = raw_symbols[sym];
        if (wrapK > 0) {
            k = ((k % wrapK) + wrapK) % wrapK;
        }
        int symbol = ((wrapK - 1) - k) & (wrapK - 1);
        int gray_value = symbol >> header_de_shift;
        if (value_mask != -1) {
            gray_value &= value_mask;
        }
        int binary_value = gray_value;
        int tmp_gray = gray_value;
        while (tmp_gray >>= 1) {
            binary_value ^= tmp_gray;
        }
#ifndef NDEBUG
        if (std::getenv("LORA_DEBUG_HEADER_STEP") != nullptr) {
            std::fprintf(stderr, "[hdr-step] sym=%d k=%d bin=%d decoded=%d\n",
                         sym, k, gray_value, binary_value);
        }
#endif
        const auto column_full = bits_from_uint(binary_value, sf);
        for (int bit = 0; bit < ppm; ++bit) {
            const int bit_val = column_full[bit];
            state.column_bits[sym * ppm + bit] = bit_val;
        }
    }

    for (int col = 0; col < n_sym_hdr; ++col) {
        for (int row = 0; row < ppm; ++row) {
            state.interleaved_columns[col][row] = state.column_bits[row + col * ppm];
        }
    }

    // Undo header interleaving/rotation.
    // Header TX mapping (mirroring GNU Radio) fills S as:
    //   for col in [0..7): for j in [0..ppm): S[col][j] = cw_bin[(col - j - 1) mod ppm][col]
    // So to recover cw_bin[row][col] at RX, we need j = (col - row - 1) mod ppm.
    // We store rows as 'row' and columns as 'col' in rotated_columns.
    for (int row = 0; row < ppm; ++row) {
        for (int col = 0; col < cw_cols; ++col) {
            const int j = positive_mod(col - row - 1, ppm);
            state.rotated_columns[row][col] = state.interleaved_columns[col][j];
        }
    }

    return state;
}

std::vector<uint8_t> build_header_nibbles(int payload_len, int cr, bool has_crc) {
    std::vector<uint8_t> hdr(5);
    hdr[0] = static_cast<uint8_t>((payload_len >> 4) & 0x0F);
    hdr[1] = static_cast<uint8_t>(payload_len & 0x0F);
    // Encode CR bits using EPFL-observed 3-bit pattern in bits [3:1] (LSB-first when read as (n2 >> 1)&7):
    // 100 -> CR=1 (4/5), 010 -> CR=2 (4/6), 110 -> CR=3 (4/7), 001 -> CR=4 (4/8)
    int cr_bits = 0b100;
    switch (std::clamp(cr, 1, 4)) {
        case 1: cr_bits = 0b100; break;
        case 2: cr_bits = 0b010; break;
        case 3: cr_bits = 0b110; break;
        case 4: cr_bits = 0b001; break;
    }
    hdr[2] = static_cast<uint8_t>(((cr_bits & 0x7) << 1) | (has_crc ? 1 : 0));
    // Compute header checksum (5 bits) using the EPFL 5x12 linear mapping over
    // [len_lo(4), len_hi(4), CR(3 LSB-first), CRCflag].
    const unsigned nib_lo = static_cast<unsigned>(hdr[1] & 0xF);
    const unsigned nib_hi = static_cast<unsigned>(hdr[0] & 0xF);
    const unsigned nib_n2 = static_cast<unsigned>(hdr[2] & 0xF);
    int b[12];
    b[0] = (nib_lo >> 0) & 1; b[1] = (nib_lo >> 1) & 1; b[2] = (nib_lo >> 2) & 1; b[3] = (nib_lo >> 3) & 1;
    b[4] = (nib_hi >> 0) & 1; b[5] = (nib_hi >> 1) & 1; b[6] = (nib_hi >> 2) & 1; b[7] = (nib_hi >> 3) & 1;
    b[8] = (nib_n2 >> 1) & 1; b[9] = (nib_n2 >> 2) & 1; b[10] = (nib_n2 >> 3) & 1; b[11] = (nib_n2 >> 0) & 1;
    // Matrix from EPFL Report (Eq. 2.15): rows correspond to crc4..crc0, columns to h11..h0
    const int M[5][12] = {
        {1,1,0,0,0,1,0,1,0,0,1,0},
        {0,1,0,1,0,0,0,1,0,1,1,0},
        {0,1,0,1,0,0,1,0,0,1,0,0},
        {1,1,0,0,0,1,0,1,0,0,0,1},
        {1,1,0,0,1,1,1,0,1,0,1,1}
    };
    auto row_parity = [&](int r) {
        int x = 0;
        for (int col = 0; col < 12; ++col) {
            if (M[r][col]) x ^= b[11 - col]; // map h11..h0 -> b[11..0]
        }
        return x & 1;
    };
    const int c4 = row_parity(0);
    const int c3 = row_parity(1);
    const int c2 = row_parity(2);
    const int c1 = row_parity(3);
    const int c0 = row_parity(4);
    hdr[3] = static_cast<uint8_t>(c4 & 1);
    hdr[4] = static_cast<uint8_t>(((c3 & 1) << 3) | ((c2 & 1) << 2) | ((c1 & 1) << 1) | (c0 & 1));
    return hdr;
}

uint8_t hamming_encode_nibble(uint8_t nibble, int cr_app) {
    const bool b3 = (nibble >> 3) & 1;
    const bool b2 = (nibble >> 2) & 1;
    const bool b1 = (nibble >> 1) & 1;
    const bool b0 = nibble & 1;

    if (cr_app == 1) {
        const bool p4 = b0 ^ b1 ^ b2 ^ b3;
        return static_cast<uint8_t>((b3 << 4) | (b2 << 3) | (b1 << 2) | (b0 << 1) | p4);
    }

    const bool p0 = b3 ^ b2 ^ b1;
    const bool p1 = b2 ^ b1 ^ b0;
    const bool p2 = b3 ^ b2 ^ b0;
    const bool p3 = b3 ^ b1 ^ b0;
    const uint8_t raw = static_cast<uint8_t>((b3 << 7) | (b2 << 6) | (b1 << 5) | (b0 << 4) |
                                             (p0 << 3) | (p1 << 2) | (p2 << 1) | p3);
    const int shift = 4 - cr_app;
    return static_cast<uint8_t>(raw >> (shift > 0 ? shift : 0));
}

} // namespace

// HeaderDecoder
// Responsibilities:
//  - Starting from frame sync result (preamble offset and CFO), locate and demodulate 8 header symbols.
//  - For each symbol: compensate CFO, dechirp, sample one per chip, and perform a K-point DFT to find raw symbol bin.
//  - Map raw symbol to header bits using the LoRa header mapping: (K-1 - k)/4, then Gray decode over (sf-2) bits.
//  - Undo LoRa's header interleaving (ppm x 8 block with column-dependent circular shifts).
//  - Hamming-decode each column with CR=4 to recover the 4-bit nibbles (single-bit correction allowed).
//  - Parse the fields: length (8 bits), n0/n1/n2 nibbles, CRC5 over (n0,n1,n2), CRC flag, CR index.
//  - Export any remaining header bits (beyond the 20 used by CRC5 and fields) for payload mapping if present.
HeaderDecoder::HeaderDecoder(int sf, int bandwidth_hz, int sample_rate_hz)
    : sf_(sf), bandwidth_hz_(bandwidth_hz), sample_rate_hz_(sample_rate_hz) {
    // Validate parameters and integer oversampling assumption.
    if (sf < 5 || sf > 12) {
        throw std::invalid_argument("Spreading factor out of supported range (5-12)");
    }
    if (bandwidth_hz <= 0 || sample_rate_hz <= 0) {
        throw std::invalid_argument("Bandwidth and sample rate must be positive");
    }
    if (sample_rate_hz % bandwidth_hz != 0) {
        throw std::invalid_argument("Sample rate must be an integer multiple of bandwidth for integer oversampling");
    }

    // Precompute sizing and the down-chirp used for dechirping.
    os_factor_ = static_cast<std::size_t>(sample_rate_hz_) / static_cast<std::size_t>(bandwidth_hz_);
    const std::size_t chips_per_symbol = static_cast<std::size_t>(1) << sf_;
    sps_ = chips_per_symbol * os_factor_;

    downchirp_ = make_downchirp(sf_, bandwidth_hz_, sample_rate_hz_);
}

std::array<int, 8> HeaderDecoder::encode_low_sf_symbols(int sf, int cr, bool has_crc, int payload_len) {
    const int sf_app = std::max(1, sf - 2);
    auto header_nibbles = build_header_nibbles(payload_len, cr, has_crc);
    std::vector<uint8_t> codewords;
    codewords.reserve(header_nibbles.size());

    for (std::size_t idx = 0; idx < header_nibbles.size(); ++idx) {
        const int cr_app = (static_cast<int>(idx) < sf_app) ? 4 : cr;
        codewords.push_back(hamming_encode_nibble(header_nibbles[idx], cr_app));
    }

    std::array<int, 8> symbols{};
    std::vector<std::vector<int>> cw_bin(sf_app, std::vector<int>(8, 0));
    for (int row = 0; row < sf_app; ++row) {
        cw_bin[row] = bits_from_uint(codewords[row], 8);
    }

    for (int col = 0; col < 8; ++col) {
        std::vector<int> column(sf, 0);
        for (int j = 0; j < sf_app; ++j) {
            const int src_row = positive_mod(col - j - 1, sf_app);
            column[j] = cw_bin[src_row][col];
        }
        const int parity = std::accumulate(column.begin(), column.begin() + sf_app, 0) & 1;
        column[sf_app] = parity;
        symbols[col] = bits_to_uint(column);
    }

    return symbols;
}

// Decode 8 header symbols given the synchronized frame start.
// Steps per symbol:
//  1) Slice N samples starting at (sync.p_ofs_est + header_offset + sym*N)
//  2) Apply CFO compensation: multiply by exp(-j*2π*cfo*Ts*(ofs+n))
//  3) Dechirp with down-chirp and pick one sample per chip (os_factor stride) at index 1+chip*os
//  4) Run K-point DFT (inverse=true here due to phase convention) and take the argmax bin
//  5) Convert argmax to raw symbol k in [0..K-1], correcting by -1 and wrapping
// Then map to bits:
//  6) Compute header bin: round((K-1 - k)/4), modulo 2^(sf-2)
//  7) Gray-decode sf-2 bits to get the column bits
//  8) Deinterleave into S (columns=8, rows=sf-2), then descramble into C using C[ii][jj]=S[jj][(ii+jj)%ppm]
//  9) Flip row order to match LoRa conventions and Hamming-decode each column with CR=4
// 10) Parse fields and verify CRC5; export extra header bits if available.
std::optional<HeaderDecodeResult> HeaderDecoder::decode(const std::vector<Sample> &samples,
                                                         const FrameSyncResult &sync) const {
    return decode(samples, sync, MutableIntSpan{}, MutableIntSpan{});
}

std::optional<HeaderDecodeResult> HeaderDecoder::decode(const std::vector<Sample> &samples,
                                                         const FrameSyncResult &sync,
                                                         MutableIntSpan header_symbols,
                                                         MutableIntSpan header_bits) const {
    const std::size_t N = sps_;
    const std::size_t K = static_cast<std::size_t>(1) << sf_;
    const double fs = static_cast<double>(sample_rate_hz_);
    const double Ts = 1.0 / fs;
    const std::size_t Nrise = static_cast<std::size_t>(std::ceil(kTriseSeconds * fs));

    // Header starts after ~12 symbols from preamble start plus a quarter-symbol offset.
    const std::size_t header_offset = Nrise + (12u * N) + (N / 4u);
    const std::ptrdiff_t base = sync.p_ofs_est + static_cast<std::ptrdiff_t>(header_offset);
    if (base < 0 || static_cast<std::size_t>(base + static_cast<std::ptrdiff_t>(8 * N)) > samples.size()) {
        return std::nullopt; // not enough samples
    }

    // Demodulate 8 raw header symbols.
    // Allocate header symbol storage lazily; reuse caller-provided buffer when supplied.
    std::array<int, 8> raw_symbol_array{};
    std::ptrdiff_t ofs = static_cast<std::ptrdiff_t>(header_offset);
    for (std::size_t sym = 0; sym < 8; ++sym) {
        // CFO rotate + dechirp
        std::vector<CDouble> temp(N);
        for (std::size_t n = 0; n < N; ++n) {
            const std::ptrdiff_t idx_signed = sync.p_ofs_est + ofs + static_cast<std::ptrdiff_t>(n);
            if (idx_signed < 0 || static_cast<std::size_t>(idx_signed) >= samples.size()) {
                return std::nullopt;
            }
            const std::size_t idx = static_cast<std::size_t>(idx_signed);
            const double angle = -2.0 * std::numbers::pi * sync.cfo_hz * Ts * static_cast<double>(ofs + static_cast<std::ptrdiff_t>(n));
            const CDouble rot = CDouble(std::cos(angle), std::sin(angle));
            temp[n] = to_cdouble(samples[idx]) * downchirp_[n] * rot;
        }

        // Sample per chip with stride os_factor, index 1+chip*os per GNU Radio convention to avoid DC
        std::vector<CFloat> dec;
        dec.reserve(K);
        for (std::size_t chip = 0; chip < K; ++chip) {
            std::size_t idx = 1 + chip * os_factor_;
            if (idx >= N - 1) {
                idx = N - 2; // clamp safely near the end
            }
            const auto &value = temp[idx];
            dec.emplace_back(static_cast<float>(value.real()),
                             static_cast<float>(value.imag()));
        }
        if (dec.size() != K) {
            return std::nullopt;
        }

    // K-point FFT (inverse=true is a phase convention matching dechirp)
    const auto spec = compute_spectrum_fft(dec, K, /*inverse=*/true, fft_scratch_);
    const std::size_t pos = argmax_abs(spec);

    // LoRa demodulators typically apply a "-1" offset after the IFFT peak search.
    int k_val = static_cast<int>(pos) - 1;
    if (k_val < 0) {
        k_val += static_cast<int>(K);
    }
#ifndef NDEBUG
    std::fprintf(stderr, "[raw-symbol] sym=%zu pos=%zu k=%d\n", sym, pos, k_val);
#endif
    raw_symbol_array[static_cast<std::size_t>(sym)] = k_val;
        ofs += static_cast<std::ptrdiff_t>(N);
    }

    auto parsed = decode_from_raw_symbols_internal(raw_symbol_array, header_bits);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    if (std::getenv("LORA_DEBUG_HEADER_STATE") != nullptr) {
        std::fprintf(stderr, "[hdr-raw] symbols=");
        for (std::size_t idx = 0; idx < raw_symbol_array.size(); ++idx) {
            std::fprintf(stderr, "%d%s", raw_symbol_array[idx],
                         idx + 1 == raw_symbol_array.size() ? "\n" : ",");
        }
    }

    HeaderDecodeResult result = std::move(parsed.value());
    result.raw_symbols.assign(raw_symbol_array.begin(), raw_symbol_array.end());

    if (header_symbols.data && header_symbols.capacity >= 8) {
        for (std::size_t i = 0; i < raw_symbol_array.size(); ++i) {
            header_symbols.data[i] = raw_symbol_array[i];
        }
        result.raw_symbol_view = std::span<const int>(header_symbols.data, raw_symbol_array.size());
    } else {
        result.raw_symbol_view = std::span<const int>(result.raw_symbols.begin(), result.raw_symbols.end());
    }

    return result;
}

std::optional<HeaderDecodeResult> HeaderDecoder::decode_from_raw_symbols(const std::array<int, 8> &raw_symbols) const {
    return decode_from_raw_symbols_internal(raw_symbols, MutableIntSpan{});
}

std::optional<HeaderDecodeResult> HeaderDecoder::decode_from_raw_symbols(const std::array<int, 8> &raw_symbols,
                                                                         MutableIntSpan header_bits) const {
    return decode_from_raw_symbols_internal(raw_symbols, header_bits);
}

std::optional<HeaderDecodeResult> HeaderDecoder::decode_from_raw_symbols_internal(const std::array<int, 8> &raw_symbol_array,
                                                                                  MutableIntSpan header_bits) const {
    const auto pipeline = build_header_pipeline(sf_, static_cast<std::size_t>(1) << sf_, raw_symbol_array);
    if (!pipeline.has_value()) {
        return std::nullopt;
    }

    const int ppm = pipeline->ppm;
    if (ppm < 3) {
        return std::nullopt;
    }

    const int CR_hdr = 4;
    const bool skip_fec = std::getenv("LORA_DEBUG_HEADER_SKIP_FEC") != nullptr;
    // Decoded rows after Hamming correction; keep the same order as rotated_columns (row 0..ppm-1).
    std::vector<std::vector<int>> rows_flip(ppm, std::vector<int>(pipeline->cw_cols, 0));

#ifndef NDEBUG
    std::fprintf(stderr, "[header-rotated]\n");
    for (int row = 0; row < ppm; ++row) {
        std::fprintf(stderr, "  rot[%d]=", row);
        for (std::size_t idx = 0; idx < pipeline->rotated_columns[row].size(); ++idx) {
            std::fprintf(stderr, "%d", pipeline->rotated_columns[row][idx] & 1);
        }
        std::fprintf(stderr, "\n");
    }
#endif

#ifndef NDEBUG
    static bool printed_expected = false;
    const bool force_print = std::getenv("LORA_DEBUG_HEADER_EXPECTED") != nullptr;
    if (force_print || !printed_expected) {
        printed_expected = true;
        const int sample_len = 11;
    const auto tx_symbols_v = encode_low_sf_symbols(sf_, /*cr=*/2, /*has_crc=*/true, sample_len);
        // Convert TX value v to the raw symbol value g the RX pipeline expects post-"pos-1":
        // g = gray(v) = v ^ (v >> 1)
        std::array<int, 8> expected_raw_k{};
        const int Kloc = 1 << sf_;
        for (int i = 0; i < 8; ++i) {
            const int v = tx_symbols_v[static_cast<std::size_t>(i)];
            const int g = v ^ (v >> 1);
            (void)Kloc;
            expected_raw_k[static_cast<std::size_t>(i)] = g;
        }
        const auto expected_pipeline = build_header_pipeline(sf_, static_cast<std::size_t>(1) << sf_, expected_raw_k);
        if (expected_pipeline.has_value()) {
            std::fprintf(stderr, "[header-rotated-expected] raw_symbols=");
            for (std::size_t idx = 0; idx < expected_raw_k.size(); ++idx) {
                std::fprintf(stderr, "%d%s", expected_raw_k[idx], idx + 1 == expected_raw_k.size() ? "" : ",");
            }
            std::fprintf(stderr, "\n");
            std::fprintf(stderr, "[header-rotated-expected]%s\n", force_print ? " (forced)" : "");
            for (int row = 0; row < expected_pipeline->ppm; ++row) {
                std::fprintf(stderr, "  exp[%d]=", row);
                for (std::size_t idx = 0; idx < expected_pipeline->rotated_columns[row].size(); ++idx) {
                    std::fprintf(stderr, "%d", expected_pipeline->rotated_columns[row][idx] & 1);
                }
                std::fprintf(stderr, "\n");
            }

            // Also print the true TX cw_bin rows for comparison to ensure our RX pipeline reconstructs them.
            {
                const int sf_app = std::max(1, sf_ - 2);
                auto hdr_nibbles = build_header_nibbles(sample_len, /*cr=*/1, /*has_crc=*/true);
                std::vector<uint8_t> cws;
                cws.reserve(hdr_nibbles.size());
                for (std::size_t i = 0; i < hdr_nibbles.size(); ++i) {
                    const int cr_app = (static_cast<int>(i) < sf_app) ? 4 : 1;
                    cws.push_back(hamming_encode_nibble(hdr_nibbles[i], cr_app));
                }
                std::vector<std::vector<int>> cw_bin(sf_app, std::vector<int>(8, 0));
                for (int r = 0; r < sf_app; ++r) cw_bin[r] = bits_from_uint(cws[static_cast<std::size_t>(r)], 8);
                std::fprintf(stderr, "[header-cw-bin-expected]\n");
                for (int r = 0; r < sf_app; ++r) {
                    std::fprintf(stderr, "  cw[%d]=", r);
                    for (int c = 0; c < 8; ++c) std::fprintf(stderr, "%d", cw_bin[r][c] & 1);
                    std::fprintf(stderr, "\n");
                }
            }
        } else {
            std::fprintf(stderr, "[header-rotated-expected] unavailable\n");
        }
    }
#endif
    for (int row = 0; row < ppm; ++row) {
    auto row_bits = pipeline->rotated_columns[row];
#ifndef NDEBUG
    std::fprintf(stderr, "[hamming-in] row[%d]=", row);
    for (std::size_t idx = 0; idx < row_bits.size(); ++idx) std::fprintf(stderr, "%d", row_bits[idx] & 1);
    std::fprintf(stderr, "\n");
#endif
    if (!skip_fec) {
        if (!hamming::decode_codeword(row_bits, CR_hdr)) {
        return std::nullopt;
        }
#ifndef NDEBUG
        std::fprintf(stderr, "[hamming-out] row[%d]=", row);
        for (std::size_t idx = 0; idx < row_bits.size(); ++idx) std::fprintf(stderr, "%d", row_bits[idx] & 1);
        std::fprintf(stderr, "\n");
#endif
    }
        rows_flip[row] = std::move(row_bits);
    }

#ifndef NDEBUG
    std::fprintf(stderr, "[header-rows] ppm=%d\n", ppm);
    for (int row = 0; row < ppm; ++row) {
        std::fprintf(stderr, "  row[%d]=", row);
        for (std::size_t idx = 0; idx < rows_flip[row].size(); ++idx) {
            std::fprintf(stderr, "%d", rows_flip[row][idx] & 1);
        }
        std::fprintf(stderr, "\n");
    }
#endif

    // Extract 4-bit nibble from a decoded row in little-endian bit order (to match bit2uint8 with bitorder='little'):
    // nibble = row[0]*1 + row[1]*2 + row[2]*4 + row[3]*8
    auto nibble_from_row = [](const std::vector<int> &row) -> int {
        // Treat the first four bits as MSB-first [d3 d2 d1 d0]
        const int d3 = (row.size() > 0 ? (row[0] & 1) : 0);
        const int d2 = (row.size() > 1 ? (row[1] & 1) : 0);
        const int d1 = (row.size() > 2 ? (row[2] & 1) : 0);
        const int d0 = (row.size() > 3 ? (row[3] & 1) : 0);
        return ((d3 << 3) | (d2 << 2) | (d1 << 1) | d0) & 0xF;
    };

    // Deterministic field mapping matching GNURadio:
    // in[0]=n0 (MS nibble of length), in[1]=n1 (LS nibble of length), in[2]=(CR<<1)|CRCflag,
    // in[3] contributes the MSB of the 5-bit checksum (its LSB), in[4] contributes the lower 4 bits of checksum.
    if (ppm < 5) {
        return std::nullopt;
    }
    // Map nibbles by row order observed in our TX builder (row 0 carries high nibble, row 1 carries low nibble).
    // This yields LENGTH = bit2uint8([C[0,0:4], C[1,0:4]]).
    const int n_hi = nibble_from_row(rows_flip[0]);
    const int n_lo = nibble_from_row(rows_flip[1]);
    const int n2 = nibble_from_row(rows_flip[2]);
    const int length = ((n_hi & 0xF) << 4) | (n_lo & 0xF);
    const int chk_rx = (((nibble_from_row(rows_flip[3]) & 0x1) << 4) | (nibble_from_row(rows_flip[4]) & 0xF)) & 0x1F;
    // CRC5 calculated over the 12 header bits: [len_lo(4), len_hi(4), (CR<<1|CRCflag)(4 but only 3+1 used)]
    // Here compute_header_crc expects (n0=MS nibble of length, n1=LS nibble of length, n2=(CR<<1)|CRCflag)
    const int chk_calc = compute_header_crc(n_hi, n_lo, n2) & 0x1F;

#ifndef NDEBUG
    std::fprintf(stderr, "[header-nibbles] length=%d n_hi=%X n_lo=%X n2=%X chk_rx=%02X chk_calc=%02X\n",
                 length, n_hi, n_lo, n2, chk_rx, chk_calc);
#endif
    if (std::getenv("LORA_DEBUG_HEADER_STATE") != nullptr) {
        std::fprintf(stderr, "[hdr-debug] length=%d n_hi=%X n_lo=%X n2=%X chk_rx=%02X chk_calc=%02X\n",
                     length, n_hi, n_lo, n2, chk_rx, chk_calc);
    }

    HeaderDecodeResult result;
    result.implicit_header = false;
    const bool has_crc_flag = (n2 & 0x1) != 0;
    const int cr_bits = (n2 >> 1) & 0x7; // 3-bit pattern as observed on air (LSB-first in nibble)
    int cr_idx = 0;
    switch (cr_bits) {
        case 0b100: cr_idx = 1; break; // 4/5
        case 0b010: cr_idx = 2; break; // 4/6
        case 0b110: cr_idx = 3; break; // 4/7
        case 0b001: cr_idx = 4; break; // 4/8
        default: cr_idx = 0; break;
    }
    if (cr_idx < 1 || cr_idx > 4) {
        return std::nullopt;
    }

    // Note: older implementation compared against an expected header matrix for sf<=6 here.
    // Removed to avoid using uninitialized fields prior to fcs_ok and assignments.

    result.fcs_ok = (ppm >= 5) ? (chk_rx == chk_calc) : true;
    if (result.fcs_ok) {
        result.payload_length = length;
        result.has_crc = has_crc_flag;
        result.cr = cr_idx;

        const int n_bits_hdr = std::max(ppm * 4 - 20, 0);
        if (n_bits_hdr > 0) {
            if (header_bits.data && header_bits.capacity >= static_cast<std::size_t>(n_bits_hdr)) {
                for (int i = 5, write_idx = 0; i < ppm && write_idx < n_bits_hdr; ++i) {
                    for (int j = 0; j < 4 && write_idx < n_bits_hdr; ++j, ++write_idx) {
                        header_bits.data[write_idx] = rows_flip[i][j];
                    }
                }
                result.payload_header_bits_view = std::span<const int>(header_bits.data, static_cast<std::size_t>(n_bits_hdr));
            } else {
                result.payload_header_bits.resize(n_bits_hdr);
                int write_idx = 0;
                for (int i = 5; i < ppm && write_idx < n_bits_hdr; ++i) {
                    for (int j = 0; j < 4 && write_idx < n_bits_hdr; ++j) {
                        result.payload_header_bits[write_idx++] = rows_flip[i][j];
                    }
                }
                result.payload_header_bits_view = std::span<const int>(result.payload_header_bits.begin(), result.payload_header_bits.end());
            }
        } else if (header_bits.data && header_bits.capacity > 0) {
            result.payload_header_bits_view = std::span<const int>(header_bits.data, std::size_t{0});
        } else {
            result.payload_header_bits_view = std::span<const int>(result.payload_header_bits.begin(), result.payload_header_bits.end());
        }
    } else {
        result.has_crc = has_crc_flag;
        result.cr = cr_idx;
        if (header_bits.data && header_bits.capacity > 0) {
            result.payload_header_bits_view = std::span<const int>(header_bits.data, std::size_t{0});
        } else {
            result.payload_header_bits_view = std::span<const int>(result.payload_header_bits.begin(), result.payload_header_bits.end());
        }
    }

    return result;
}

std::size_t HeaderDecoder::symbol_span_samples() const {
    return 8u * sps_;
}

// Compute the 5-bit LoRa header checksum (CRC5) as specified by the bit relations over n0,n1,n2.
int HeaderDecoder::compute_header_crc(int n_hi, int n_lo, int n2) {
    const unsigned nib_lo = static_cast<unsigned>(n_lo) & 0xF;
    const unsigned nib_hi = static_cast<unsigned>(n_hi) & 0xF;
    const unsigned nib_n2 = static_cast<unsigned>(n2) & 0xF;
    int b[12];
    b[0] = (nib_lo >> 0) & 1; b[1] = (nib_lo >> 1) & 1; b[2] = (nib_lo >> 2) & 1; b[3] = (nib_lo >> 3) & 1;
    b[4] = (nib_hi >> 0) & 1; b[5] = (nib_hi >> 1) & 1; b[6] = (nib_hi >> 2) & 1; b[7] = (nib_hi >> 3) & 1;
    b[8] = (nib_n2 >> 1) & 1; b[9] = (nib_n2 >> 2) & 1; b[10] = (nib_n2 >> 3) & 1; b[11] = (nib_n2 >> 0) & 1;

    // Use matrix as published in EPFL (Eq. 2.15) mapping h11..h0 to crc4..crc0.
    const int M[5][12] = {
        {1,1,0,0,0,1,0,1,0,0,1,0},
        {0,1,0,1,0,0,0,1,0,1,1,0},
        {0,1,0,1,0,0,1,0,0,1,0,0},
        {1,1,0,0,0,1,0,1,0,0,0,1},
        {1,1,0,0,1,1,1,0,1,0,1,1}
    };
    auto row_parity = [&](int r) {
        int x = 0;
        for (int col = 0; col < 12; ++col) if (M[r][col]) x ^= b[11 - col];
        return x & 1;
    };
    const int c4 = row_parity(0);
    const int c3 = row_parity(1);
    const int c2 = row_parity(2);
    const int c1 = row_parity(3);
    const int c0 = row_parity(4);
    return ((c4 & 1) << 4) | ((c3 & 1) << 3) | ((c2 & 1) << 2) | ((c1 & 1) << 1) | (c0 & 1);
}

} // namespace lora
