#include "header_decoder.hpp"

#include "chirp_generator.hpp"
#include "hamming.hpp"
#include "fft_utils.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace lora {

namespace {

// Small front-end rise-time compensation used to align the header start similar to the reference.
constexpr double kTriseSeconds = 50e-6;

using CDouble = std::complex<double>;

// Cast input sample typedef to complex<double> for numerics.
CDouble to_cdouble(const HeaderDecoder::Sample &sample) {
    return CDouble(static_cast<double>(sample.real()), static_cast<double>(sample.imag()));
}

// FFT utility using lora::fft with optional inverse sign; zero-pads to fft_len.
std::vector<CDouble> compute_spectrum_fft(const std::vector<CDouble> &input, std::size_t fft_len, bool inverse = false) {
    std::vector<CDouble> spectrum = input;
    spectrum.resize(fft_len, CDouble{0.0, 0.0});
    lora::fft::transform_pow2(spectrum, inverse);
    return spectrum;
}

// Argmax by magnitude helper for spectral peaks.
std::size_t argmax_abs(const std::vector<CDouble> &vec) {
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

// Build a Gray-decode lookup of size 2^(sf-2); LoRa header uses sf-2 bits after divide-by-4.
std::vector<int> lora_degray_table(int bits) {
    const int size = 1 << bits;
    std::vector<int> decode(size, 0);
    for (int value = 0; value < size; ++value) {
        int prev_bit = 0;
        int accum = 0;
        for (int row = 0; row < bits; ++row) {
            const int shift = bits - 1 - row;
            const int bit = (value >> shift) & 1;
            const int mapped = (bit + prev_bit) & 1; // reverse Gray: cumulative XOR
            accum = (accum << 1) | mapped;
            prev_bit = bit;
        }
        decode[value] = accum;
    }
    return decode;
}

// Pack little-endian bit vector into an integer.
int bits_to_uint_le(const std::vector<int> &bits) {
    int value = 0;
    for (std::size_t i = 0; i < bits.size(); ++i) {
        value |= (bits[i] & 1) << static_cast<int>(i);
    }
    return value;
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
    std::vector<int> raw_symbols;
    raw_symbols.reserve(8);

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
        std::vector<CDouble> dec;
        dec.reserve(K);
        for (std::size_t chip = 0; chip < K; ++chip) {
            std::size_t idx = 1 + chip * os_factor_;
            if (idx >= N - 1) {
                idx = N - 2; // clamp safely near the end
            }
            dec.push_back(temp[idx]);
        }
        if (dec.size() != K) {
            return std::nullopt;
        }

    // K-point FFT (inverse=true is a phase convention matching dechirp)
    const auto spec = compute_spectrum_fft(dec, K, /*inverse=*/true);
        const std::size_t pos = argmax_abs(spec);

        // Convert DFT bin to raw symbol index
        int k_val = static_cast<int>(pos) - 1;
        if (k_val < 0) {
            k_val += static_cast<int>(K);
        }
        raw_symbols.push_back(k_val);
        ofs += static_cast<std::ptrdiff_t>(N);
    }

    // Header uses sf-2 bits per symbol after divide-by-4 mapping.
    const int ppm = std::max(1, sf_ - 2);
    const int n_sym_hdr = 8;
    const int CR_hdr = 4;
    const int cw_cols = 4 + CR_hdr; // 8 columns per LoRa header nibble+parity layout

    if (ppm < 5) {
        return std::nullopt; // header needs at least 5 rows (sf>=7)
    }

    // Gray lookup for ppm bits
    const auto degray = lora_degray_table(ppm);

    // Map raw symbols to header bits: round((K-1 - k)/4) mod 2^ppm, then Gray-decode
    std::vector<int> bits_est(ppm * n_sym_hdr, 0);
    const double K_minus_1 = static_cast<double>(K - 1);

    for (int sym = 0; sym < n_sym_hdr; ++sym) {
        const double val = static_cast<double>(raw_symbols[sym]);
        const double bin_d = std::round((K_minus_1 - val) / 4.0);
        int bin = static_cast<int>(bin_d);
        const int mask = (1 << ppm) - 1;
        bin = ((bin % (1 << ppm)) + (1 << ppm)) % (1 << ppm); // wrap to [0,2^ppm)
        const int decoded = degray[bin & mask];
        for (int bit = 0; bit < ppm; ++bit) {
            const int bit_val = (decoded >> (ppm - 1 - bit)) & 1; // MSB-first per column
            bits_est[sym * ppm + bit] = bit_val;
        }
    }

    // Build S (columns=8, rows=ppm) in column-major order
    std::vector<std::vector<int>> S(n_sym_hdr, std::vector<int>(ppm, 0));
    for (int col = 0; col < n_sym_hdr; ++col) {
        for (int row = 0; row < ppm; ++row) {
            S[col][row] = bits_est[row + col * ppm];
        }
    }

    // Descramble to C using column-dependent circular shifts: C[ii][jj] = S[jj][(ii + jj) % ppm]
    std::vector<std::vector<int>> C(ppm, std::vector<int>(cw_cols, 0));
    for (int ii = 0; ii < ppm; ++ii) {
        for (int jj = 0; jj < cw_cols; ++jj) {
            C[ii][jj] = S[jj][(ii + jj) % ppm];
        }
    }

    // Flip rows to match the Hamming decoder expectation (top-bottom inversion)
    std::vector<std::vector<int>> C_flip = C;
    for (int row = 0; row < ppm; ++row) {
        C_flip[row] = C[ppm - 1 - row];
        if (!hamming::decode_codeword(C_flip[row], CR_hdr)) {
            return std::nullopt; // uncorrectable error
        }
    }

    // Extract fields
    // Length (8 bits) from rows {1,0}: little-endian packing of 4+4 bits
    std::vector<int> len_bits;
    len_bits.reserve(8);
    for (int i = 0; i < 4; ++i) { len_bits.push_back(C_flip[1][i]); }
    for (int i = 0; i < 4; ++i) { len_bits.push_back(C_flip[0][i]); }
    const int length = bits_to_uint_le(len_bits) & 0xFF;

    // Nibbles n0,n1,n2 (each 4b)
    std::vector<int> n0_bits(C_flip[0].begin(), C_flip[0].begin() + 4);
    std::vector<int> n1_bits(C_flip[1].begin(), C_flip[1].begin() + 4);
    std::vector<int> n2_bits(C_flip[2].begin(), C_flip[2].begin() + 4);

    const int n0 = bits_to_uint_le(n0_bits) & 0xF;
    const int n1 = bits_to_uint_le(n1_bits) & 0xF;
    const int n2 = bits_to_uint_le(n2_bits) & 0xF;

    // Header CRC5 over (n0,n1,n2)
    std::vector<int> fcs_bits;
    fcs_bits.reserve(8);
    for (int i = 0; i < 4; ++i) { fcs_bits.push_back(C_flip[4][i]); }
    for (int i = 0; i < 4; ++i) { fcs_bits.push_back(C_flip[3][i]); }
    const int fcs_hdr = bits_to_uint_le(fcs_bits) & 0xFF;
    const int chk_rx = fcs_hdr & 0x1F;
    const int chk_calc = compute_header_crc(n0, n1, n2) & 0x1F;

    // Populate result
    HeaderDecodeResult result;
    result.implicit_header = false;
    result.raw_symbols = raw_symbols;
    result.fcs_ok = (chk_rx == chk_calc);
    if (result.fcs_ok) {
        result.payload_length = length;
        result.has_crc = (n2 & 0x1) != 0;
        result.cr = (n2 >> 1) & 0x7;
        // Export extra header bits (beyond 20) for payload mapping, if any
        const int n_bits_hdr = std::max(ppm * 4 - 20, 0);
        if (n_bits_hdr > 0) {
            result.payload_header_bits.resize(n_bits_hdr);
            int write_idx = 0;
            for (int i = 5; i < ppm; ++i) {
                for (int j = 0; j < 4 && write_idx < n_bits_hdr; ++j) {
                    result.payload_header_bits[write_idx++] = C_flip[i][j];
                }
            }
        }
    }
    return result;
}

// Compute the 5-bit LoRa header checksum (CRC5) as specified by the bit relations over n0,n1,n2.
int HeaderDecoder::compute_header_crc(int n0, int n1, int n2) {
    n0 &= 0xF;
    n1 &= 0xF;
    n2 &= 0xF;
    const int c4 = ((n0 >> 3) & 1) ^ ((n0 >> 2) & 1) ^ ((n0 >> 1) & 1) ^ (n0 & 1);
    const int c3 = ((n0 >> 3) & 1) ^ ((n1 >> 3) & 1) ^ ((n1 >> 2) & 1) ^ ((n1 >> 1) & 1) ^ (n2 & 1);
    const int c2 = ((n0 >> 2) & 1) ^ ((n1 >> 3) & 1) ^ (n1 & 1) ^ ((n2 >> 3) & 1) ^ ((n2 >> 1) & 1);
    const int c1 = ((n0 >> 1) & 1) ^ ((n1 >> 2) & 1) ^ (n1 & 1) ^ ((n2 >> 2) & 1) ^ ((n2 >> 1) & 1) ^ (n2 & 1);
    const int c0 = (n0 & 1) ^ ((n1 >> 1) & 1) ^ ((n2 >> 3) & 1) ^ ((n2 >> 2) & 1) ^ ((n2 >> 1) & 1) ^ (n2 & 1);
    return ((c4 & 1) << 4) | ((c3 & 1) << 3) | ((c2 & 1) << 2) | ((c1 & 1) << 1) | (c0 & 1);
}

} // namespace lora
