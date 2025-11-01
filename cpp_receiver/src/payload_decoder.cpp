#include "payload_decoder.hpp"

#include "chirp_generator.hpp"
#include "fft_utils.hpp"
#include "hamming.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <span>
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
using CFloat = std::complex<float>;

// Convert a 16-bit value into an array of MSB-first bits; used by the CRC helper.
std::array<int, 16> bits_from_uint16(uint16_t value) {
    std::array<int, 16> bits{};
    for (int i = 0; i < 16; ++i) {
        const int shift = 15 - i;
        bits[i] = (value >> shift) & 1;
    }
    return bits;
}

// Argmax by magnitude helper for spectral peaks.
template <typename Complex>
std::size_t argmax_abs(const std::vector<Complex> &vec) {
    std::size_t idx = 0;
    double max_mag = -1.0;
    for (std::size_t i = 0; i < vec.size(); ++i) {
        const double mag = std::norm(vec[i]);
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

constexpr std::array<std::array<int, 5>, 4> kGeneratorP1{{
    {1, 0, 0, 0, 1},
    {0, 1, 0, 0, 1},
    {0, 0, 1, 0, 1},
    {0, 0, 0, 1, 1},
}};

constexpr std::array<std::array<int, 8>, 4> kGeneratorPMax{{
    {1, 0, 0, 0, 1, 0, 1, 1},
    {0, 1, 0, 0, 1, 1, 1, 0},
    {0, 0, 1, 0, 1, 1, 0, 1},
    {0, 0, 0, 1, 0, 1, 1, 1},
}};

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
    downchirp_f_.resize(downchirp_.size());
    for (std::size_t i = 0; i < downchirp_.size(); ++i) {
        downchirp_f_[i] = std::complex<float>(static_cast<float>(downchirp_[i].real()),
                                              static_cast<float>(downchirp_[i].imag()));
    }
    downchirp_decimated_.resize(chips_per_symbol);
    for (std::size_t k = 0; k < chips_per_symbol; ++k) {
        downchirp_decimated_[k] = downchirp_f_[k * os_factor_];
    }
}

// Compute payload start offset (samples) relative to preamble start.
// In explicit mode: preamble + 12.25 symbols + 8 header symbols.
// In implicit (embedded) mode: preamble + 12.25 symbols (no header symbols transmitted).
std::size_t PayloadDecoder::payload_symbol_offset_samples(bool implicit_header) const {
    const std::size_t N = sps_;
    const std::size_t Nrise = static_cast<std::size_t>(std::ceil(kTriseSeconds * static_cast<double>(sample_rate_hz_)));
    const std::size_t header_syms = implicit_header ? 0u : 8u;
    return Nrise + (12u * N) + (N / 4u) + header_syms * N;
}

// Lazily build and cache the inverse Gray mapping table for the requested bit width.
const std::vector<int> &PayloadDecoder::get_degray_table(int bits) const {
    if (bits <= 0) {
        static const std::vector<int> kEmpty{};
        return kEmpty;
    }

#ifdef LORA_EMBEDDED_PROFILE
    const std::size_t size = static_cast<std::size_t>(1) << bits;
    if (bits != degray_bits_) {
        for (std::size_t value = 0; value < size; ++value) {
            int prev_binary = 0;
            int accum = 0;
            for (int row = 0; row < bits; ++row) {
                const int shift = bits - 1 - row;
                const int gray_bit = (static_cast<int>(value) >> shift) & 1;
                const int binary_bit = gray_bit ^ prev_binary; // standard Gray decode recurrence
                accum = (accum << 1) | binary_bit;
                prev_binary = binary_bit;
            }
            degray_table_[value] = accum;
        }
        degray_bits_ = bits;
    }
    static std::vector<int> view;
    view.assign(degray_table_.begin(), degray_table_.begin() + (static_cast<std::size_t>(1) << bits));
    return view;
#else
    const std::size_t size = static_cast<std::size_t>(1) << bits;
    if (bits != degray_bits_ || degray_table_.size() != size) {
        degray_table_.assign(size, 0);
        for (std::size_t value = 0; value < size; ++value) {
            int prev_binary = 0;
            int accum = 0;
            for (int row = 0; row < bits; ++row) {
                const int shift = bits - 1 - row;
                const int gray_bit = (static_cast<int>(value) >> shift) & 1;
                const int binary_bit = gray_bit ^ prev_binary; // accumulate binary bits: b[i] = g[i] XOR b[i-1]
                accum = (accum << 1) | binary_bit;
                prev_binary = binary_bit;
            }
            degray_table_[value] = accum;
        }
        degray_bits_ = bits;
    }
    return degray_table_;
#endif
}

// Read 8 little-endian bits starting at offset into a byte.
unsigned PayloadDecoder::byte_from_bits(const std::vector<int> &bits, std::size_t offset) const {
    unsigned value = 0;
    for (std::size_t i = 0; i < 8; ++i) { value |= (static_cast<unsigned>(bits[offset + i] & 1) << i); }
    return value & 0xFFu;
}

// Remove LoRa whitening (self-synchronizing scrambler). Seeds with all ones and advances per byte.
void PayloadDecoder::dewhiten_bits(std::vector<int> &bits, std::size_t bit_count) const {
    std::array<int, 8> W{1, 1, 1, 1, 1, 1, 1, 1}; // Whitening shift-register state.
    const std::array<int, 8> W_fb{0, 0, 0, 1, 1, 1, 0, 1};
    if (bit_count == 0) {
        return;
    }
    const std::size_t byte_count = (bit_count + 7u) / 8u;
    for (std::size_t k = 0; k < byte_count; ++k) {
        std::size_t base = k * 8;
        for (std::size_t j = 0; j < 8; ++j) {
            const std::size_t idx = base + j;
            if (idx >= bit_count) {
                break;
            }
            bits[idx] = (bits[idx] + W[j]) & 1;
        }
        int sum = 0; for (std::size_t j = 0; j < 8; ++j) { sum += W[j] * W_fb[j]; }
        const int W1 = sum & 1;
        for (std::size_t j = 7; j > 0; --j) { W[j] = W[j - 1]; }
        W[0] = W1;
    }
}

std::array<int, 8> PayloadDecoder::encode_codeword_from_data(int data_bits, int parity_bits) const {
    std::array<int, 8> codeword{};
    const int cols = std::clamp(parity_bits, 0, 4) + 4;
    for (int col = 0; col < cols; ++col) {
        int value = 0;
        if (parity_bits <= 0) {
            value = (data_bits >> col) & 1;
        } else if (parity_bits == 1) {
            for (int row = 0; row < 4; ++row) {
                if (((data_bits >> row) & 1) != 0) {
                    value ^= kGeneratorP1[row][col];
                }
            }
        } else {
            for (int row = 0; row < 4; ++row) {
                if (((data_bits >> row) & 1) != 0) {
                    value ^= kGeneratorPMax[row][col];
                }
            }
        }
        codeword[static_cast<std::size_t>(col)] = value & 1;
    }
    return codeword;
}

bool PayloadDecoder::decode_codeword_soft(const float *llr, int parity_bits, std::array<int, 8> &decoded_codeword) const {
    const int cols = std::clamp(parity_bits, 0, 4) + 4;
    if (llr == nullptr) {
        return false;
    }

    float best_metric = std::numeric_limits<float>::lowest();
    int best_data = 0;
    std::array<int, 8> best_codeword{};

    for (int data = 0; data < 16; ++data) {
        const auto candidate = encode_codeword_from_data(data, parity_bits);
        float metric = 0.0f;
        for (int col = 0; col < cols; ++col) {
            const float L = llr[col];
            const int bit = candidate[static_cast<std::size_t>(col)] & 1;
            metric += 0.5f * L * (bit ? 1.0f : -1.0f);
        }
        if (metric > best_metric) {
            best_metric = metric;
            best_data = data;
            best_codeword = candidate;
        }
    }

    decoded_codeword = best_codeword;
    (void)best_data;
    return true;
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

    auto init_bits = bits_from_uint16(state_vec[length - 5]);
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

    const int ih = header.implicit_header ? 1 : 0;
    const int denom = std::max(1, 4 * (sf - 2 * de));
    int numerator = 8 * payload_len - 4 * sf + 28 + 16 * crc - 20 * ih;
    if (numerator < 0) {
        numerator = 0;
    }
    const int ceil_term = (numerator + denom - 1) / denom;
    const int sym_per_block = 4 + cr;
    const int variable_symbols = sym_per_block * std::max(ceil_term, 0);

    // Always include the fixed first interleaver block (8 symbols at CR=4/8).
    return 8 + variable_symbols;
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
                                                          bool ldro_enabled,
                                                          bool soft_decoding) const {
    return decode(samples, sync, header, ldro_enabled, soft_decoding, MutableByteSpan{}, MutableIntSpan{});
}

std::optional<PayloadDecodeResult> PayloadDecoder::decode(const std::vector<Sample> &samples,
                                                          const FrameSyncResult &sync,
                                                          const HeaderDecodeResult &header,
                                                          bool ldro_enabled,
                                                          bool soft_decoding,
                                                          MutableByteSpan external_payload,
                                                          MutableIntSpan external_raw_symbols) const {
    if ((!header.implicit_header && !header.fcs_ok) || header.payload_length <= 0) {
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
    const double sr = (sync.sample_rate_ratio > 0.0) ? sync.sample_rate_ratio : 1.0;

    const int n_payload_syms = compute_payload_symbol_count(header, ldro_enabled);
    if (n_payload_syms <= 0) {
#ifndef NDEBUG
        std::fprintf(stderr, "[decode] n_payload_syms<=0\n");
#endif
        return std::nullopt;
    }

    struct BlockParams {
        int symbol_count = 0;
        int cr = 0;
        bool ldro = false;
        int de = 0;
        int ppm = 0;
    };

    std::vector<BlockParams> blocks;
    {
        int symbols_remaining = n_payload_syms;
        if (symbols_remaining > 0) {
            BlockParams first;
            first.symbol_count = std::min(symbols_remaining, 8);
            first.cr = 4;
            const bool force_ldro_block0 = (sf_ > 6);
            first.ldro = (sf_ <= 6) ? false : (force_ldro_block0 ? true : ldro_enabled);
            first.de = (sf_ > 10 || first.ldro) ? 1 : 0;
            first.ppm = std::max(0, sf_ - 2 * first.de);
            blocks.push_back(first);
            symbols_remaining -= first.symbol_count;
        }
        while (symbols_remaining > 0) {
            BlockParams block;
            block.symbol_count = std::min(symbols_remaining, 4 + cr);
            block.cr = cr;
            block.ldro = ldro_enabled;
            block.de = (sf_ > 10 || block.ldro) ? 1 : 0;
            block.ppm = std::max(0, sf_ - 2 * block.de);
            blocks.push_back(block);
            symbols_remaining -= block.symbol_count;
        }
    }

    std::vector<int> raw_symbols_storage;
    std::span<int> raw_symbols;
    if (external_raw_symbols.data && external_raw_symbols.capacity >= static_cast<std::size_t>(n_payload_syms)) {
        raw_symbols = std::span<int>(external_raw_symbols.data, static_cast<std::size_t>(n_payload_syms));
    } else {
        raw_symbols_storage.resize(static_cast<std::size_t>(n_payload_syms));
        raw_symbols = std::span<int>(raw_symbols_storage.begin(), raw_symbols_storage.end());
    }

    std::vector<int> symbol_bins_storage(raw_symbols.size(), 0);
    std::vector<int> degray_values_storage(raw_symbols.size(), 0);

    std::size_t total_soft_bits = 0;
    std::size_t payload_bits_only = 0;
    for (const auto &block : blocks) {
        total_soft_bits += static_cast<std::size_t>(block.ppm) * static_cast<std::size_t>(block.symbol_count);
        payload_bits_only += static_cast<std::size_t>(block.ppm) * 4u;
    }
    const bool use_soft = soft_decoding && (total_soft_bits > 0);

    std::vector<std::size_t> symbol_llr_offset(static_cast<std::size_t>(n_payload_syms) + 1, 0);
    if (use_soft) {
        block_llr_buffer_.assign(total_soft_bits, 0.0f);
    } else {
        block_llr_buffer_.clear();
    }

    double symbol_cursor = static_cast<double>(sync.p_ofs_est) + static_cast<double>(symbol_offset) * sr;
    const double first_sample = symbol_cursor;
    if (first_sample < 0.0 || first_sample >= static_cast<double>(samples.size())) {
#ifndef NDEBUG
        std::fprintf(stderr, "[payload] start out of range first=%f size=%zu\n",
                     first_sample, samples.size());
#endif
        return std::nullopt;
    }

    auto fetch_sample = [&](double cursor, CFloat &out) -> bool {
        const auto size = static_cast<std::ptrdiff_t>(samples.size());
        if (size <= 0) {
            return false;
        }
        std::ptrdiff_t idx0 = static_cast<std::ptrdiff_t>(cursor);
        if (idx0 < 0) {
            return false;
        }
        if (idx0 >= size) {
            idx0 = size - 1;
        }
        std::ptrdiff_t idx1 = idx0 + 1;
        if (idx1 >= size) {
            idx1 = idx0;
        }
        const double frac = cursor - static_cast<double>(idx0);
        const Sample &s0 = samples[static_cast<std::size_t>(idx0)];
        const Sample &s1 = samples[static_cast<std::size_t>(idx1)];
        const float frac_f = static_cast<float>(frac);
        out = (1.0f - frac_f) * s0 + frac_f * s1;
        return true;
    };
    const std::size_t K = static_cast<std::size_t>(1) << sf_;

    decimated_buffer_.resize(K);

    const double chip_step_samples = sr * static_cast<double>(os_factor_);
    const float chip_step_time_f = static_cast<float>(chip_step_samples * Ts);
    const float cfo_f = static_cast<float>(sync.cfo_hz);

    std::size_t block_index = 0;
    int symbol_in_block = 0;
    std::size_t soft_offset = 0;

    for (int sym = 0; sym < n_payload_syms; ++sym) {
        while (block_index < blocks.size() && symbol_in_block >= blocks[block_index].symbol_count) {
            ++block_index;
            symbol_in_block = 0;
        }
        if (block_index >= blocks.size()) {
#ifndef NDEBUG
            std::fprintf(stderr, "[payload] block index overflow\n");
#endif
            return std::nullopt;
        }
        const BlockParams &block = blocks[block_index];
        const int block_ppm = block.ppm;
        const int block_de = block.de;
        const bool symbol_use_soft = use_soft && (block_ppm > 0);
        if (use_soft) {
            symbol_llr_offset[static_cast<std::size_t>(sym)] = soft_offset;
        }

        const auto &degray_block = get_degray_table(block_ppm);

        const double base_time = (symbol_cursor - static_cast<double>(sync.p_ofs_est)) * Ts;
        const float base_time_f = static_cast<float>(base_time);
        const float angle0 = -2.0f * std::numbers::pi_v<float> * cfo_f * base_time_f;
        const float angle_step = -2.0f * std::numbers::pi_v<float> * cfo_f * chip_step_time_f;
        float rot_re = std::cos(angle0);
        float rot_im = std::sin(angle0);
        const float step_re = std::cos(angle_step);
        const float step_im = std::sin(angle_step);

        for (std::size_t k = 0; k < K; ++k) {
            const double cursor = symbol_cursor + static_cast<double>(k) * chip_step_samples;
            CFloat interpolated;
            if (!fetch_sample(cursor, interpolated)) {
#ifndef NDEBUG
                std::fprintf(stderr, "[payload] sample idx out of range (cursor=%f size=%zu)\n",
                             cursor, samples.size());
#endif
                return std::nullopt;
            }
            const std::complex<float> down = downchirp_decimated_[k];
            const float s_re = interpolated.real();
            const float s_im = interpolated.imag();
            const float d_re = down.real();
            const float d_im = down.imag();
            const float t_re = s_re * d_re - s_im * d_im;
            const float t_im = s_re * d_im + s_im * d_re;
            const float out_re = t_re * rot_re - t_im * rot_im;
            const float out_im = t_re * rot_im + t_im * rot_re;
            decimated_buffer_[k] = CFloat(out_re, out_im);
            const float new_rot_re = rot_re * step_re - rot_im * step_im;
            const float new_rot_im = rot_re * step_im + rot_im * step_re;
            rot_re = new_rot_re;
            rot_im = new_rot_im;
        }

        lora::fft::transform_pow2(decimated_buffer_, /*inverse=*/true, fft_scratch_);
        if (symbol_use_soft) {
            const std::size_t state_count = static_cast<std::size_t>(1) << static_cast<std::size_t>(block_ppm);
            symbol_metric_buffer_.assign(state_count, 0.0f);
            const std::size_t llr_base = soft_offset;
            for (std::size_t state = 0; state < state_count; ++state) {
                int symbol_index = static_cast<int>(state) << (2 * block_de);
                symbol_index &= static_cast<int>(K) - 1;
                int fft_index = symbol_index + 1;
                if (fft_index >= static_cast<int>(K)) {
                    fft_index -= static_cast<int>(K);
                }
                const float mag = std::abs(decimated_buffer_[static_cast<std::size_t>(fft_index)]);
                const float safe_mag = std::max(mag, 1e-12f);
                symbol_metric_buffer_[state] = std::log(safe_mag);
            }
            for (int bit = 0; bit < block_ppm; ++bit) {
                float best_one = std::numeric_limits<float>::lowest();
                float best_zero = std::numeric_limits<float>::lowest();
                for (std::size_t state = 0; state < symbol_metric_buffer_.size(); ++state) {
                    const int decoded_val = degray_block[state];
                    const float metric = symbol_metric_buffer_[state];
                    if (((decoded_val >> bit) & 1) != 0) {
                        if (metric > best_one) {
                            best_one = metric;
                        }
                    } else {
                        if (metric > best_zero) {
                            best_zero = metric;
                        }
                    }
                }
                block_llr_buffer_[llr_base + static_cast<std::size_t>(bit)] = best_one - best_zero;
            }
            soft_offset += static_cast<std::size_t>(block_ppm);
        }

        std::size_t pos = argmax_abs(decimated_buffer_);
        int k_val = static_cast<int>(pos) - 1;
        if (k_val < 0) { k_val += static_cast<int>(K); }
        raw_symbols[static_cast<std::size_t>(sym)] = k_val;
        symbol_cursor += static_cast<double>(N) * sr;
        ++symbol_in_block;
    }
    if (use_soft) {
        symbol_llr_offset[static_cast<std::size_t>(n_payload_syms)] = soft_offset;
    }

    payload_bits_buffer_.clear();
    if (!header.implicit_header) {
        payload_bits_buffer_.insert(payload_bits_buffer_.end(), header.payload_header_bits.begin(), header.payload_header_bits.end());
    }
    const std::size_t header_bit_count = payload_bits_buffer_.size();
    payload_bits_buffer_.resize(header_bit_count + payload_bits_only);
    std::size_t payload_ofs = header_bit_count;

    block_bits_buffer_.clear();
    interleave_buffer_s_.clear();
    interleave_buffer_c_.clear();
    interleave_buffer_s_llr_.clear();
    interleave_buffer_c_llr_.clear();

    std::size_t block_symbol_start = 0;
    for (std::size_t blk = 0; blk < blocks.size(); ++blk) {
        const BlockParams &block = blocks[blk];
        const int block_ppm = block.ppm;
        const int block_n_sym = block.symbol_count;
        const int block_cr = block.cr;
        const int codeword_len = block_cr + 4;
        const int bin_mask = (block_ppm > 0) ? ((1 << block_ppm) - 1) : 0;

        block_bits_buffer_.assign(static_cast<std::size_t>(block_ppm) * static_cast<std::size_t>(block_n_sym), 0);
        interleave_buffer_s_.assign(static_cast<std::size_t>(block_n_sym) * static_cast<std::size_t>(block_ppm), 0);
        interleave_buffer_c_.assign(static_cast<std::size_t>(block_ppm) * static_cast<std::size_t>(block_n_sym), 0);
        if (use_soft && block_ppm > 0) {
            interleave_buffer_s_llr_.assign(static_cast<std::size_t>(block_n_sym) * static_cast<std::size_t>(block_ppm), 0.0f);
            interleave_buffer_c_llr_.assign(static_cast<std::size_t>(block_ppm) * static_cast<std::size_t>(block_n_sym), 0.0f);
        }

        const auto &degray_block = get_degray_table(block_ppm);
        int *bits_blk = block_bits_buffer_.data();
        float *S_llr = (use_soft && block_ppm > 0) ? interleave_buffer_s_llr_.data() : nullptr;
        for (int sym = 0; sym < block_n_sym; ++sym) {
            const std::size_t idx = block_symbol_start + static_cast<std::size_t>(sym);
            const int k_val = raw_symbols[idx];
            const int symbol_value = wrap_mod(k_val, static_cast<int>(K));
            const int reduced = (block.de > 0) ? (symbol_value >> (2 * block.de)) : symbol_value;
            const int bin = (block_ppm > 0) ? (reduced & bin_mask) : 0;
            const int decoded = (block_ppm > 0) ? degray_block[bin] : 0;
            symbol_bins_storage[idx] = bin;
            degray_values_storage[idx] = decoded;
            for (int bit = 0; bit < block_ppm; ++bit) {
                bits_blk[sym * block_ppm + bit] = (decoded >> bit) & 1;
                if (S_llr) {
                    const std::size_t llr_base = symbol_llr_offset[idx];
                    S_llr[sym * block_ppm + bit] = block_llr_buffer_[llr_base + static_cast<std::size_t>(bit)];
                }
            }
        }

        int *S = interleave_buffer_s_.data();
        int *C = interleave_buffer_c_.data();
        for (int row = 0; row < block_n_sym; ++row) {
            for (int col = 0; col < block_ppm; ++col) {
                S[row * block_ppm + col] = bits_blk[row * block_ppm + col];
            }
        }
        float *S_llr_ptr = (use_soft && block_ppm > 0) ? interleave_buffer_s_llr_.data() : nullptr;
        float *C_llr_ptr = (use_soft && block_ppm > 0) ? interleave_buffer_c_llr_.data() : nullptr;
        for (int col = 0; col < block_n_sym; ++col) {
            for (int bit = 0; bit < block_ppm; ++bit) {
                const int row = wrap_mod(col - bit - 1, block_ppm);
                C[row * block_n_sym + col] = S[col * block_ppm + bit];
                if (C_llr_ptr && S_llr_ptr) {
                    C_llr_ptr[row * block_n_sym + col] = S_llr_ptr[col * block_ppm + bit];
                }
            }
        }

        codeword_buffer_.assign(static_cast<std::size_t>(codeword_len), 0);
        if (use_soft && block_ppm > 0) {
            codeword_llr_buffer_.assign(static_cast<std::size_t>(codeword_len), 0.0f);
        }

        for (int row = 0; row < block_ppm; ++row) {
            for (int col = 0; col < block_n_sym && col < codeword_len; ++col) {
                codeword_buffer_[static_cast<std::size_t>(col)] = C[row * block_n_sym + col] & 1;
                if (use_soft && block_ppm > 0 && col < codeword_len) {
                    codeword_llr_buffer_[static_cast<std::size_t>(col)] = interleave_buffer_c_llr_[row * block_n_sym + col];
                }
            }
            if (block_cr > 0) {
                if (use_soft && block_ppm > 0) {
                    std::array<int, 8> decoded_codeword{};
                    [[maybe_unused]] const bool soft_ok =
                        decode_codeword_soft(codeword_llr_buffer_.data(), block_cr, decoded_codeword);
                    for (int col = 0; col < codeword_len; ++col) {
                        codeword_buffer_[static_cast<std::size_t>(col)] = decoded_codeword[static_cast<std::size_t>(col)] & 1;
                    }
                } else {
                    [[maybe_unused]] const bool corrected = hamming::decode_codeword(codeword_buffer_, block_cr);
#ifndef NDEBUG
                    if (!corrected) {
                        std::fprintf(stderr, "[payload-fec] decode failure blk=%zu row=%d\n", blk, row);
                    }
#endif
                }
            }

            for (int col = 0; col < 4; ++col) {
                if (payload_ofs >= payload_bits_buffer_.size()) {
#ifndef NDEBUG
                    std::fprintf(stderr, "[decode] payload_ofs overflow (%zu >= %zu)\n",
                                 payload_ofs, payload_bits_buffer_.size());
#endif
                    return std::nullopt;
                }
                payload_bits_buffer_[payload_ofs++] = codeword_buffer_[static_cast<std::size_t>(col)] & 1;
            }
        }

        block_symbol_start += static_cast<std::size_t>(block_n_sym);
    }

    if (payload_ofs != payload_bits_buffer_.size()) {
#ifndef NDEBUG
        std::fprintf(stderr, "[decode] payload bit count mismatch (%zu != %zu)\n",
                     payload_ofs, payload_bits_buffer_.size());
#endif
        return std::nullopt;
    }

    // 3) Remove whitening (payload bytes only; CRC remains untouched) and pack bytes.
    const std::size_t message_bit_count = static_cast<std::size_t>(header.payload_length) * 8u;
    const std::size_t bits_to_dewhiten = std::min<std::size_t>(payload_bits_buffer_.size(),
                                                               header_bit_count + message_bit_count);
    dewhiten_bits(payload_bits_buffer_, bits_to_dewhiten);

    std::size_t total_bits = payload_bits_buffer_.size();
    if (total_bits % 8 != 0) {
        const std::size_t padded = ((total_bits + 7u) / 8u) * 8u;
        payload_bits_buffer_.resize(padded, 0);
        total_bits = padded;
    }
    const std::size_t total_bytes = total_bits / 8;
    const std::size_t payload_length = static_cast<std::size_t>(header.payload_length);
    if (total_bytes < payload_length) {
#ifndef NDEBUG
        std::fprintf(stderr, "[decode] total_bytes < payload_length (%zu < %zu)\n", total_bytes, payload_length);
#endif
        return std::nullopt;
    }

    // Convert the bit stream into bytes using either caller-provided storage or the internal buffer.
    std::vector<unsigned char> payload_storage;
    std::span<unsigned char> payload_bytes;
    if (external_payload.data && external_payload.capacity >= total_bytes) {
        payload_bytes = std::span<unsigned char>(external_payload.data, total_bytes);
    } else {
        payload_storage.assign(total_bytes, 0);
        payload_bytes = std::span<unsigned char>(payload_storage.begin(), payload_storage.end());
    }
    for (std::size_t i = 0; i < total_bytes; ++i) {
        payload_bytes[i] = static_cast<unsigned char>(byte_from_bits(payload_bits_buffer_, i * 8));
    }

    // 4) Extract message and check optional CRC16.
    std::vector<unsigned char> message(payload_bytes.begin(), payload_bytes.begin() + payload_length);
    bool crc_ok = true;
    if (header.has_crc) {
        if (payload_length < 2u) {
            crc_ok = false;
        } else {
            const std::size_t data_bytes = payload_length - 2u;
            const std::size_t data_bits = data_bytes * 8u;
            const uint16_t observed_crc = static_cast<uint16_t>(payload_bytes[payload_length - 2]) |
                                          (static_cast<uint16_t>(payload_bytes[payload_length - 1]) << 8);

            // Re-encode CRC per LoRa rules: compute polynomial over the message (sans CRC),
            // then XOR the resulting 16 bits with the last two payload bytes.
            const auto crc_bits = crc16_bits(payload_bits_buffer_, data_bits);
            uint16_t computed_crc = 0;
            for (int i = 0; i < 16; ++i) {
                computed_crc |= static_cast<uint16_t>(crc_bits[i] & 1) << i;
            }
            const uint16_t folded_crc = computed_crc ^
                                        (static_cast<uint16_t>(message[message.size() - 2]) |
                                         (static_cast<uint16_t>(message[message.size() - 1]) << 8));

            crc_ok = (observed_crc == folded_crc);
        }
    }

    PayloadDecodeResult result;
    result.raw_symbols = std::move(raw_symbols_storage);
    result.bytes = std::move(message);
    result.crc_ok = crc_ok;
    result.symbol_bins = std::move(symbol_bins_storage);
    result.degray_values = std::move(degray_values_storage);
    if (external_raw_symbols.data && external_raw_symbols.capacity >= static_cast<std::size_t>(n_payload_syms)) {
        result.raw_symbol_view = raw_symbols;
    } else {
        result.raw_symbol_view = std::span<const int>(result.raw_symbols.begin(), result.raw_symbols.end());
    }
    if (external_payload.data && external_payload.capacity >= total_bytes) {
        result.byte_view = payload_bytes;
    } else {
        result.byte_view = std::span<const unsigned char>(result.bytes.begin(), result.bytes.end());
    }
    if (!result.symbol_bins.empty()) {
        result.symbol_bin_view = std::span<const int>(result.symbol_bins.begin(), result.symbol_bins.end());
    }
    if (!result.degray_values.empty()) {
        result.degray_view = std::span<const int>(result.degray_values.begin(), result.degray_values.end());
    }
    return result;
}

} // namespace lora
