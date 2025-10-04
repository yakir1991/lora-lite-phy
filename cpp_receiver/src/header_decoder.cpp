#include "header_decoder.hpp"

#include "chirp_generator.hpp"
#include "hamming.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace lora {

namespace {

constexpr double kTriseSeconds = 50e-6;

using CDouble = std::complex<double>;

CDouble to_cdouble(const HeaderDecoder::Sample &sample) {
    return CDouble(static_cast<double>(sample.real()), static_cast<double>(sample.imag()));
}

std::vector<CDouble> compute_dft(const std::vector<CDouble> &input, std::size_t fft_len, bool inverse = false) {
    std::vector<CDouble> spectrum(fft_len, CDouble{0.0, 0.0});
    const double sign = inverse ? 1.0 : -1.0;
    for (std::size_t k = 0; k < fft_len; ++k) {
        CDouble acc{0.0, 0.0};
        const double coeff = sign * 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(fft_len);
        for (std::size_t n = 0; n < input.size(); ++n) {
            const double angle = coeff * static_cast<double>(n);
            acc += input[n] * CDouble(std::cos(angle), std::sin(angle));
        }
        spectrum[k] = acc;
    }
    return spectrum;
}

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

std::vector<int> lora_degray_table(int bits) {
    const int size = 1 << bits;
    std::vector<int> decode(size, 0);
    for (int value = 0; value < size; ++value) {
        int prev_bit = 0;
        int accum = 0;
        for (int row = 0; row < bits; ++row) {
            const int shift = bits - 1 - row;
            const int bit = (value >> shift) & 1;
            const int mapped = (bit + prev_bit) & 1;
            accum = (accum << 1) | mapped;
            prev_bit = bit;
        }
        decode[value] = accum;
    }
    return decode;
}

int bits_to_uint_le(const std::vector<int> &bits) {
    int value = 0;
    for (std::size_t i = 0; i < bits.size(); ++i) {
        value |= (bits[i] & 1) << static_cast<int>(i);
    }
    return value;
}

} // namespace

HeaderDecoder::HeaderDecoder(int sf, int bandwidth_hz, int sample_rate_hz)
    : sf_(sf), bandwidth_hz_(bandwidth_hz), sample_rate_hz_(sample_rate_hz) {
    if (sf < 5 || sf > 12) {
        throw std::invalid_argument("Spreading factor out of supported range (5-12)");
    }
    if (bandwidth_hz <= 0 || sample_rate_hz <= 0) {
        throw std::invalid_argument("Bandwidth and sample rate must be positive");
    }
    if (sample_rate_hz % bandwidth_hz != 0) {
        throw std::invalid_argument("Sample rate must be an integer multiple of bandwidth for integer oversampling");
    }

    os_factor_ = static_cast<std::size_t>(sample_rate_hz_) / static_cast<std::size_t>(bandwidth_hz_);
    const std::size_t chips_per_symbol = static_cast<std::size_t>(1) << sf_;
    sps_ = chips_per_symbol * os_factor_;

    downchirp_ = make_downchirp(sf_, bandwidth_hz_, sample_rate_hz_);
}

std::optional<HeaderDecodeResult> HeaderDecoder::decode(const std::vector<Sample> &samples,
                                                         const FrameSyncResult &sync) const {
    const std::size_t N = sps_;
    const std::size_t K = static_cast<std::size_t>(1) << sf_;
    const double fs = static_cast<double>(sample_rate_hz_);
    const double Ts = 1.0 / fs;
    const std::size_t Nrise = static_cast<std::size_t>(std::ceil(kTriseSeconds * fs));

    const std::size_t header_offset = Nrise + (12u * N) + (N / 4u);
    const std::ptrdiff_t base = sync.p_ofs_est + static_cast<std::ptrdiff_t>(header_offset);
    if (base < 0 || static_cast<std::size_t>(base + static_cast<std::ptrdiff_t>(8 * N)) > samples.size()) {
        return std::nullopt;
    }

    std::vector<int> raw_symbols;
    raw_symbols.reserve(8);

    std::ptrdiff_t ofs = static_cast<std::ptrdiff_t>(header_offset);
    for (std::size_t sym = 0; sym < 8; ++sym) {
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

        std::vector<CDouble> dec;
        dec.reserve(K);
        for (std::size_t chip = 0; chip < K; ++chip) {
            std::size_t idx = 1 + chip * os_factor_;
            if (idx >= N - 1) {
                idx = N - 2;
            }
            dec.push_back(temp[idx]);
        }
        if (dec.size() != K) {
            return std::nullopt;
        }

        const auto spec = compute_dft(dec, K, true);
        const std::size_t pos = argmax_abs(spec);
        int k_val = static_cast<int>(pos) - 1;
        if (k_val < 0) {
            k_val += static_cast<int>(K);
        }
        raw_symbols.push_back(k_val);
        ofs += static_cast<std::ptrdiff_t>(N);
    }

    const int ppm = std::max(1, sf_ - 2);
    const int n_sym_hdr = 8;
    const int CR_hdr = 4;
    const int cw_cols = 4 + CR_hdr;

    if (ppm < 5) {
        return std::nullopt;
    }

    const auto degray = lora_degray_table(ppm);
    std::vector<int> bits_est(ppm * n_sym_hdr, 0);
    const double K_minus_1 = static_cast<double>(K - 1);

    for (int sym = 0; sym < n_sym_hdr; ++sym) {
        const double val = static_cast<double>(raw_symbols[sym]);
        const double bin_d = std::round((K_minus_1 - val) / 4.0);
        int bin = static_cast<int>(bin_d);
        const int mask = (1 << ppm) - 1;
        bin = ((bin % (1 << ppm)) + (1 << ppm)) % (1 << ppm);
        const int decoded = degray[bin & mask];
        for (int bit = 0; bit < ppm; ++bit) {
            const int bit_val = (decoded >> (ppm - 1 - bit)) & 1;
            bits_est[sym * ppm + bit] = bit_val;
        }
    }

    std::vector<std::vector<int>> S(n_sym_hdr, std::vector<int>(ppm, 0));
    for (int col = 0; col < n_sym_hdr; ++col) {
        for (int row = 0; row < ppm; ++row) {
            S[col][row] = bits_est[row + col * ppm];
        }
    }

    std::vector<std::vector<int>> C(ppm, std::vector<int>(cw_cols, 0));
    for (int ii = 0; ii < ppm; ++ii) {
        for (int jj = 0; jj < cw_cols; ++jj) {
            C[ii][jj] = S[jj][(ii + jj) % ppm];
        }
    }

    std::vector<std::vector<int>> C_flip = C;
    for (int row = 0; row < ppm; ++row) {
        C_flip[row] = C[ppm - 1 - row];
        if (!hamming::decode_codeword(C_flip[row], CR_hdr)) {
            return std::nullopt;
        }
    }


    std::vector<int> len_bits;
    len_bits.reserve(8);
    for (int i = 0; i < 4; ++i) {
        len_bits.push_back(C_flip[1][i]);
    }
    for (int i = 0; i < 4; ++i) {
        len_bits.push_back(C_flip[0][i]);
    }
    const int length = bits_to_uint_le(len_bits) & 0xFF;

    std::vector<int> n0_bits(C_flip[0].begin(), C_flip[0].begin() + 4);
    std::vector<int> n1_bits(C_flip[1].begin(), C_flip[1].begin() + 4);
    std::vector<int> n2_bits(C_flip[2].begin(), C_flip[2].begin() + 4);

    const int n0 = bits_to_uint_le(n0_bits) & 0xF;
    const int n1 = bits_to_uint_le(n1_bits) & 0xF;
    const int n2 = bits_to_uint_le(n2_bits) & 0xF;

    std::vector<int> fcs_bits;
    fcs_bits.reserve(8);
    for (int i = 0; i < 4; ++i) {
        fcs_bits.push_back(C_flip[4][i]);
    }
    for (int i = 0; i < 4; ++i) {
        fcs_bits.push_back(C_flip[3][i]);
    }
    const int fcs_hdr = bits_to_uint_le(fcs_bits) & 0xFF;
    const int chk_rx = fcs_hdr & 0x1F;
    const int chk_calc = compute_header_crc(n0, n1, n2) & 0x1F;

    HeaderDecodeResult result;
    result.implicit_header = false;
    result.raw_symbols = raw_symbols;
    result.fcs_ok = (chk_rx == chk_calc);
    if (result.fcs_ok) {
        result.payload_length = length;
        result.has_crc = (n2 & 0x1) != 0;
        result.cr = (n2 >> 1) & 0x7;
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
