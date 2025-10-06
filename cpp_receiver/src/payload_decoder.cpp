#include "payload_decoder.hpp"

#include "chirp_generator.hpp"
#include "fft_utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <stdexcept>

// Payload decoding stitches together many LoRa quirks: symbol alignment via the
// fine sync estimate, CFO removal, Gray de-mapping with optional LDRO scaling,
// block interleaver undo, whitening, and CRC verification. This file keeps those
// steps in one place so the data flow is easy to trace while still exposing
// enough helpers for unit tests to poke individual pieces.

namespace lora {

namespace {

// Small front-end rise-time used in aligning payload start similarly to the reference chain.
constexpr double kTriseSeconds = 50e-6;

using CDouble = std::complex<double>;

// Cast input sample typedef to complex<double> for numerics.
CDouble to_cdouble(const PayloadDecoder::Sample &sample) {
    return CDouble(static_cast<double>(sample.real()), static_cast<double>(sample.imag()));
}

// FFT-based spectrum: use lora::fft::transform_pow2 on a copy of input (complex<double>),
// performing an inverse-like transform to align with the previous IDFT convention.
std::vector<CDouble> compute_spectrum_fft(std::vector<CDouble> input, std::size_t fft_len, bool inverse = true) {
    // Pad/trim to fft_len (should already be exact K=2^SF)
    input.resize(fft_len, CDouble{0.0, 0.0});
    std::vector<CDouble> spectrum = input;
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

// Wrap integer modulo in [0,mod).
int wrap_mod(int value, int mod) {
    const int m = mod;
    int r = value % m;
    if (r < 0) { r += m; }
    return r;
}

} // namespace

// PayloadDecoder
// Responsibilities:
//  - Compute where payload symbols start (relative to preamble/header) and how many symbols to demodulate.
//  - For each symbol: CFO-rotate, dechirp, one-sample-per-chip, DFT(K), argmax -> raw symbol index k.
//  - Convert raw symbols to bits using LoRa mapping: divide-by-2^(2*DE), Gray-decode over (sf-2*DE) bits.
//  - Deinterleave/descramble per LoRa block structure (rows=ppm, cols=4+CR) and collect nibble bits.
//  - Concatenate header-provided bits (or fake header bits in implicit mode) before dewhitening.
//  - Dewhiten, pack bytes, and verify optional CRC16.
PayloadDecoder::PayloadDecoder(int sf, int bandwidth_hz, int sample_rate_hz)
    : sf_(sf), bandwidth_hz_(bandwidth_hz), sample_rate_hz_(sample_rate_hz) {
    // Validate parameters and integer oversampling.
    if (sf < 5 || sf > 12) { throw std::invalid_argument("Spreading factor out of supported range (5-12)"); }
    if (bandwidth_hz <= 0 || sample_rate_hz <= 0) { throw std::invalid_argument("Bandwidth and sample rate must be positive"); }
    if (sample_rate_hz % bandwidth_hz != 0) { throw std::invalid_argument("Sample rate must be integer multiple of bandwidth"); }

    // Derived sizes and reference chirp for dechirping.
    os_factor_ = static_cast<std::size_t>(sample_rate_hz_) / static_cast<std::size_t>(bandwidth_hz_);
    const std::size_t chips_per_symbol = static_cast<std::size_t>(1) << sf_;
    sps_ = chips_per_symbol * os_factor_;

    downchirp_ = make_downchirp(sf_, bandwidth_hz_, sample_rate_hz_);
}

// Compute payload start offset (samples) relative to preamble start.
// Note: Even in implicit-header mode we keep the same offset as explicit (preamble + 12.25 sym + 8 header sym)
// so the payload indexing remains consistent; we adjust only the bit accounting logic later.
std::size_t PayloadDecoder::payload_symbol_offset_samples(bool implicit_header) const {
    const std::size_t N = sps_;
    const std::size_t Nrise = static_cast<std::size_t>(std::ceil(kTriseSeconds * static_cast<double>(sample_rate_hz_)));
    // Always use explicit header offset - implicit header should work exactly like explicit
    return Nrise + (12u * N) + (N / 4u) + 8u * N;
}

// Gray decode lookup for `bits` width; LoRa payload mapping uses ppm = sf-2*DE bits per symbol.
std::vector<int> PayloadDecoder::lora_degray_table(int bits) const {
    const int size = 1 << bits;
    std::vector<int> decode(size, 0);
    for (int value = 0; value < size; ++value) {
        int prev_bit = 0;
        int accum = 0;
        for (int row = 0; row < bits; ++row) {
            const int shift = bits - 1 - row;
            const int bit = (value >> shift) & 1;
            const int mapped = (bit + prev_bit) & 1; // reverse Gray accumulation
            accum = (accum << 1) | mapped;
            prev_bit = bit;
        }
        decode[value] = accum;
    }
    return decode;
}

// Convert integer to MSB-first bit vector of length bit_count.
std::vector<int> PayloadDecoder::num_to_bits(unsigned value, int bit_count) const {
    std::vector<int> bits(bit_count, 0);
    for (int i = 0; i < bit_count; ++i) {
        const int shift = bit_count - 1 - i;
        bits[i] = static_cast<int>((value >> shift) & 1U);
    }
    return bits;
}

// Read 8 little-endian bits starting at offset into a byte.
unsigned PayloadDecoder::byte_from_bits(const std::vector<int> &bits, std::size_t offset) const {
    unsigned value = 0;
    for (std::size_t i = 0; i < 8; ++i) { value |= (static_cast<unsigned>(bits[offset + i] & 1) << i); }
    return value & 0xFFu;
}

// Remove LoRa whitening (self-synchronizing scrambler). Seeds with all ones and advances per byte.
std::vector<int> PayloadDecoder::dewhiten_bits(const std::vector<int> &bits) const {
    std::vector<int> out(bits);
    std::array<int, 8> W{1, 1, 1, 1, 1, 1, 1, 1};
    const std::array<int, 8> W_fb{0, 0, 0, 1, 1, 1, 0, 1};
    const std::size_t byte_count = out.size() / 8;
    for (std::size_t k = 0; k < byte_count; ++k) {
        std::size_t base = k * 8;
        for (std::size_t j = 0; j < 8; ++j) { out[base + j] = (out[base + j] + W[j]) & 1; }
        int sum = 0; for (std::size_t j = 0; j < 8; ++j) { sum += W[j] * W_fb[j]; }
        const int W1 = sum & 1;
        for (std::size_t j = 7; j > 0; --j) { W[j] = W[j - 1]; }
        W[0] = W1;
    }
    return out;
}

// Compute payload CRC-16 bits for a given message-bit length using the LoRa-specific state-vec method.
std::array<int, 16> PayloadDecoder::crc16_bits(const std::vector<int> &bits, std::size_t bit_count) const {
    static constexpr std::array<uint16_t, 251> state_vec = {
        46885, 27367, 35014, 54790, 18706, 15954, 9784, 59350, 12042, 22321,
        46211, 20984, 56450, 7998, 62433, 35799, 2946, 47628, 30930, 52144,
        59061, 10600, 56648, 10316, 34962, 55618, 57666, 2088, 61160, 25930,
        63354, 24012, 29658, 17909, 41022, 17072, 42448, 5722, 10472, 56651,
        40183, 19835, 21851, 13020, 35306, 42553, 12394, 57960, 8434, 25101,
        63814, 29049, 27264, 213, 13764, 11996, 46026, 6259, 8758, 22513,
        43163, 38423, 62727, 60460, 29548, 18211, 6559, 61900, 55362, 46606,
        19928, 6028, 35232, 29422, 28379, 55218, 38956, 12132, 49339, 47243,
        39300, 53336, 29575, 53957, 5941, 63650, 9502, 28329, 44510, 28068,
        19538, 19577, 36943, 59968, 41464, 33923, 54504, 49962, 64357, 12382,
        44678, 11234, 58436, 47434, 63636, 51152, 29296, 61176, 33231, 32706,
        27862, 11005, 41129, 38527, 32824, 20579, 37742, 22493, 37464, 56698,
        29428, 27269, 7035, 27911, 55897, 50485, 10543, 38817, 54183, 52989,
        24549, 33562, 8963, 38328, 13330, 24139, 5996, 8270, 49703, 60444,
        8277, 43598, 1693, 60789, 32523, 36522, 17339, 33912, 23978, 55777,
        34725, 2990, 13722, 60616, 61229, 19060, 58889, 43920, 9043, 10131,
        26896, 8918, 64347, 42307, 42863, 7853, 4844, 60762, 21736, 62423,
        53096, 19242, 55756, 26615, 53246, 11257, 2844, 47011, 10022, 13541,
        18296, 44005, 23544, 18733, 23770, 33147, 5237, 45754, 4432, 22560,
        40752, 50620, 32260, 2407, 26470, 2423, 33831, 34260, 1057, 552,
        56487, 62909, 4753, 7924, 40021, 7849, 4895, 10401, 32039, 40207,
        63952, 10156, 53647, 51938, 16861, 46769, 7703, 9288, 33345, 16184,
        56808, 30265, 10696, 4218, 7708, 32139, 34174, 32428, 20665, 3869,
        43003, 6609, 60431, 22531, 11704, 63584, 13620, 14292, 37000, 8503,
        38414, 38738, 10517, 48783, 30506, 63444, 50520, 34666, 341, 34793,
        2623
    };

    const std::size_t length = bit_count / 8;
    if (length < 5 || (length - 5) >= state_vec.size()) { return std::array<int, 16>{}; }

    auto init_bits = num_to_bits(state_vec[length - 5], 16);
    std::array<int, 16> crc_tmp{}; for (int i = 0; i < 16; ++i) { crc_tmp[i] = init_bits[i]; }

    int pos = 0, pos4 = 4, pos11 = 11;
    for (std::size_t j = 0; j < length; ++j) {
        for (int k = 0; k < 8; ++k) {
            const int bit_idx = static_cast<int>(j * 8 + (7 - k));
            const int add = crc_tmp[pos];
            crc_tmp[pos] = (bit_idx < static_cast<int>(bits.size())) ? bits[bit_idx] : 0;
            if (add) { crc_tmp[pos4] ^= 1; crc_tmp[pos11] ^= 1; crc_tmp[pos] ^= 1; }
            pos = (pos + 1) % 16; pos4 = (pos4 + 1) % 16; pos11 = (pos11 + 1) % 16;
        }
    }

    std::array<int, 16> crc_bits{};
    for (int idx = 15; idx >= 0; --idx) { const int src = (pos + idx) % 16; crc_bits[15 - idx] = crc_tmp[src]; }
    return crc_bits;
}

// Compute number of payload symbols based on header fields and LDRO (DE) setting.
// - For explicit header: matches LoRa formula ceil((8*PL - 4*SF + 28 + 16*CRC) / (4*(SF - 2*DE))) * (CR+4)
// - For implicit header: we still count 20 header bits to keep whitening alignment, then compute blocks.
int PayloadDecoder::compute_payload_symbol_count(const HeaderDecodeResult &header, bool ldro_enabled) const {
    const int sf = sf_;
    const bool force_ldro = ldro_enabled;
    const int de = (force_ldro || sf >= 11) ? 1 : 0;
    const int cr = std::min(std::max(header.cr, 1), 4);
    const int crc = header.has_crc ? 1 : 0;
    const int payload_len = std::max(header.payload_length, 0);

    if (header.implicit_header) {
        // For implicit header: work exactly like explicit header - always assume 20 header bits
        const int ppm = sf - 2 * de;
        const int n_bits_blk = ppm * 4;  // bits per block (28 for SF=7)
        const int n_bits_tot = 8 * payload_len + 16 * crc;  // message + CRC bits
        const int n_bits_hdr = 20;  // Always 20 header bits
        
        // Calculate number of blocks needed (ceiling division)
        const int n_blk_tot = (n_bits_tot - n_bits_hdr + n_bits_blk - 1) / n_bits_blk;
        const int sym_per_block = 4 + cr;
        const int symbols = sym_per_block * std::max(n_blk_tot, 0);
        return symbols;
    } else {
        // Original explicit header calculation
        const int ih = 0; // explicit header path
        const int denom = std::max(1, 4 * (sf - 2 * de));
        int numerator = 8 * payload_len - 4 * sf + 28 + 16 * crc - 20 * ih;
        if (numerator < 0) { numerator = 0; }
        const int ceil_term = (numerator + denom - 1) / denom;
        const int sym_per_block = 4 + cr;
        const int symbols = sym_per_block * std::max(ceil_term, 0);
        return symbols;
    }
}

// End-to-end payload demod and decode.
// Inputs:
//  - samples, sync result, header fields, ldro flag
// Outputs:
//  - raw symbol bins, decoded bytes, CRC status (if present)
// Edge cases handled:
//  - Sample bounds, zero/negative symbol counts, size mismatches mid-pipeline, CRC length checks
std::optional<PayloadDecodeResult> PayloadDecoder::decode(const std::vector<Sample> &samples,
                                                          const FrameSyncResult &sync,
                                                          const HeaderDecodeResult &header,
                                                          bool ldro_enabled) const {
    if (!header.fcs_ok || header.payload_length <= 0) {
#ifndef NDEBUG
        std::fprintf(stderr, "[decode] header invalid\n");
#endif
        return std::nullopt;
    }
    const int cr = std::clamp(header.cr, 1, 4);
    const std::size_t N = sps_;
    const std::size_t symbol_offset = payload_symbol_offset_samples(header.implicit_header);
    const double fs = static_cast<double>(sample_rate_hz_);
    const double Ts = 1.0 / fs;

    const int n_payload_syms = compute_payload_symbol_count(header, ldro_enabled);
    if (n_payload_syms <= 0) {
#ifndef NDEBUG
        std::fprintf(stderr, "[decode] n_payload_syms<=0\n");
#endif
        return std::nullopt;
    }

    // 1) Demodulate raw symbol indices for the payload section.
    std::vector<int> raw_symbols; raw_symbols.reserve(static_cast<std::size_t>(n_payload_syms));

    std::ptrdiff_t ofs = static_cast<std::ptrdiff_t>(symbol_offset);
    const std::size_t K = static_cast<std::size_t>(1) << sf_;

    for (int sym = 0; sym < n_payload_syms; ++sym) {
        // CFO rotate + dechirp current symbol window
        std::vector<CDouble> temp(N);
        for (std::size_t n = 0; n < N; ++n) {
            const std::ptrdiff_t idx_signed = sync.p_ofs_est + ofs + static_cast<std::ptrdiff_t>(n);
            if (idx_signed < 0 || static_cast<std::size_t>(idx_signed) >= samples.size()) {
#ifndef NDEBUG
                std::fprintf(stderr, "[decode] sample idx out of range (idx=%lld size=%zu)\n",
                           static_cast<long long>(idx_signed), samples.size());
#endif
                return std::nullopt;
            }
            const double angle = -2.0 * std::numbers::pi * sync.cfo_hz * Ts * static_cast<double>(ofs + static_cast<std::ptrdiff_t>(n));
            const CDouble rot(std::cos(angle), std::sin(angle));
            temp[n] = to_cdouble(samples[static_cast<std::size_t>(idx_signed)]) * downchirp_[n] * rot;
        }

        // Take one sample per chip (stride = os_factor_)
        std::vector<CDouble> dec; dec.reserve(K);
        for (std::size_t i = 0; i < N; i += os_factor_) { dec.push_back(temp[i]); }
        if (dec.size() != K) {
#ifndef NDEBUG
            std::fprintf(stderr, "[decode] dec size mismatch %zu vs %zu\n", dec.size(), K);
#endif
            return std::nullopt;
        }

    // FFT(K) and argmax -> raw symbol bin (inverse=true to match previous sign convention)
    const auto spec = compute_spectrum_fft(dec, K, true);
        std::size_t pos = argmax_abs(spec);
        int k_val = static_cast<int>(pos) - 1; if (k_val < 0) { k_val += static_cast<int>(K); }
        raw_symbols.push_back(k_val);
        ofs += static_cast<std::ptrdiff_t>(N);
    }

    // 2) Map raw symbols to bits per LoRa payload rule.
    const int de = (sf_ > 10 || ldro_enabled) ? 1 : 0; // data rate optimization (LDRO)
    const int ppm = sf_ - 2 * de;                       // bits per symbol
    const int n_sym_blk = 4 + cr;                       // symbols per interleaving block
    const std::size_t n_blk_tot = raw_symbols.size() / static_cast<std::size_t>(n_sym_blk);
    const std::size_t n_bits_blk = static_cast<std::size_t>(ppm) * 4u; // bits per block

    // Assemble payload bits: prepend header-provided bits (or fake ones in implicit mode) for whitening alignment.
    std::vector<int> payload_bits;
    
    if (header.implicit_header) {
        // For implicit header, insert fake header bits for dewhitening compatibility (20 bits)
        payload_bits.reserve(20 + n_blk_tot * n_bits_blk);
        std::vector<int> fake_header_bits = {1,1,1,0,1,1,0,1,1,1,0,1,1,1,0,1,0,0,0,0};
        payload_bits.insert(payload_bits.end(), fake_header_bits.begin(), fake_header_bits.end());
    } else {
        payload_bits.reserve(header.payload_header_bits.size() + n_blk_tot * n_bits_blk);
        payload_bits.insert(payload_bits.end(), header.payload_header_bits.begin(), header.payload_header_bits.end());
    }

    const auto degray = lora_degray_table(ppm);
    const double pow_scale = std::pow(2.0, 2 * de); // divide-by-4 per DE=1 (i.e., 2^(2*DE))

    std::size_t payload_ofs = payload_bits.size();
    payload_bits.resize(payload_bits.size() + n_blk_tot * n_bits_blk);

    for (std::size_t blk = 0; blk < n_blk_tot; ++blk) {
        // Convert each symbol in the block to `ppm` bits via inverse Gray, with DE scaling.
        std::vector<int> bits_blk(static_cast<std::size_t>(ppm) * n_sym_blk, 0);
        for (int sym = 0; sym < n_sym_blk; ++sym) {
            const std::size_t idx = blk * static_cast<std::size_t>(n_sym_blk) + static_cast<std::size_t>(sym);
            int k_val = raw_symbols[idx];
            const double numerator = static_cast<double>(K) - 2.0 - static_cast<double>(k_val);
            const int bin = wrap_mod(static_cast<int>(std::llround(numerator / pow_scale)), 1 << ppm);
            const int decoded = degray[bin];
            const auto nibble_bits = num_to_bits(static_cast<unsigned>(decoded), ppm);
            for (int bit = 0; bit < ppm; ++bit) { bits_blk[sym * ppm + bit] = nibble_bits[bit]; }
        }

        // S: row-major [symbol][bit pos], then descramble into C with column-dependent circular shift
        std::vector<std::vector<int>> S(n_sym_blk, std::vector<int>(ppm, 0));
        for (int row = 0; row < n_sym_blk; ++row) {
            for (int col = 0; col < ppm; ++col) { S[row][col] = bits_blk[row * ppm + col]; }
        }

        std::vector<std::vector<int>> C(ppm, std::vector<int>(n_sym_blk, 0));
        for (int ii = 0; ii < ppm; ++ii) {
            for (int jj = 0; jj < n_sym_blk; ++jj) {
                const int src_col = (ii + jj) % ppm; // column shift per LoRa interleaver
                C[ii][jj] = S[jj][src_col];
            }
        }

        // Flip rows (top-bottom) to match downstream bit ordering
        for (int row = 0; row < ppm / 2; ++row) { std::swap(C[row], C[ppm - 1 - row]); }

        // Extract 4 data bits per symbol (first 4 columns of each row) into payload_bits
        for (int row = 0; row < ppm; ++row) {
            for (int col = 0; col < 4; ++col) {
                if (payload_ofs >= payload_bits.size()) {
#ifndef NDEBUG
                    std::fprintf(stderr, "[decode] payload_ofs overflow (%zu >= %zu)\n", payload_ofs, payload_bits.size());
#endif
                    return std::nullopt;
                }
                payload_bits[payload_ofs++] = C[row][col];
            }
        }
    }

    // 3) Remove whitening and pack bytes.
    payload_bits = dewhiten_bits(payload_bits);

    std::size_t total_bits = payload_bits.size();
    if (total_bits % 8 != 0) { const std::size_t padded = ((total_bits + 7u) / 8u) * 8u; payload_bits.resize(padded, 0); total_bits = padded; }
    const std::size_t total_bytes = total_bits / 8;
    const std::size_t payload_length = static_cast<std::size_t>(header.payload_length);
    if (total_bytes < payload_length) {
#ifndef NDEBUG
        std::fprintf(stderr, "[decode] total_bytes < payload_length (%zu < %zu)\n", total_bytes, payload_length);
#endif
        return std::nullopt;
    }

    std::vector<unsigned char> bytes(total_bytes, 0);
    for (std::size_t i = 0; i < total_bytes; ++i) { bytes[i] = static_cast<unsigned char>(byte_from_bits(payload_bits, i * 8)); }

    // 4) Extract message and check optional CRC16.
    std::vector<unsigned char> message(bytes.begin(), bytes.begin() + payload_length);
    bool crc_ok = true;
    if (header.has_crc) {
        const std::size_t message_bit_count = payload_length * 8u;
        if (payload_bits.size() < message_bit_count + 16u) {
            crc_ok = false;
        } else {
            auto crc_calc_bits = crc16_bits(payload_bits, message_bit_count);
            std::array<int, 16> crc_obs_bits{}; for (int i = 0; i < 16; ++i) { crc_obs_bits[i] = payload_bits[message_bit_count + static_cast<std::size_t>(i)] & 1; }
            crc_ok = std::equal(crc_obs_bits.begin(), crc_obs_bits.end(), crc_calc_bits.begin());
        }
    }

    PayloadDecodeResult result;
    result.raw_symbols = std::move(raw_symbols);
    result.bytes = std::move(message);
    result.crc_ok = crc_ok;
    return result;
}

} // namespace lora
