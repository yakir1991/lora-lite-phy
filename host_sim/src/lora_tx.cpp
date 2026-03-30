#include "host_sim/chirp.hpp"
#include "host_sim/gray.hpp"
#include "host_sim/lora_params.hpp"
#include "host_sim/lora_replay/header_encoder.hpp"
#include "host_sim/lora_replay/stage_processing.hpp"
#include "host_sim/whitening.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace
{

// ---------- helpers ----------

int modulo(int a, int m)
{
    int result = a % m;
    if (result < 0) result += m;
    return result;
}

std::vector<bool> uint_to_bits(uint16_t value, int bit_count)
{
    std::vector<bool> bits(bit_count);
    for (int i = 0; i < bit_count; ++i) {
        bits[bit_count - 1 - i] = ((value >> i) & 0x1u) != 0;
    }
    return bits;
}

uint16_t bits_to_uint(const std::vector<bool>& bits)
{
    uint16_t value = 0;
    for (bool bit : bits) {
        value = static_cast<uint16_t>((value << 1) | (bit ? 1u : 0u));
    }
    return value;
}

uint16_t gray_encode(uint16_t v) { return v ^ (v >> 1); }

// ---------- Hamming encode ----------
// Produces (4+cr)-bit codeword from a 4-bit nibble.
// Matches GnuRadio hamming_enc_impl: data bits LSB-first, then parity bits.
// cr=1 → parity only (5 bits); cr=2..4 → Hamming variants.
uint8_t hamming_encode(uint8_t nibble, int cr)
{
    const bool d3 = ((nibble >> 3) & 1) != 0;
    const bool d2 = ((nibble >> 2) & 1) != 0;
    const bool d1 = ((nibble >> 1) & 1) != 0;
    const bool d0 = (nibble & 1) != 0;

    if (cr == 1) {
        const bool p = d3 ^ d2 ^ d1 ^ d0;
        return static_cast<uint8_t>((d0 << 4) | (d1 << 3) | (d2 << 2) | (d3 << 1) | p);
    }

    // cr >= 2: data bits LSB-first + parity bits
    const bool p0 = d0 ^ d1 ^ d2;
    const bool p1 = d1 ^ d2 ^ d3;
    const bool p2 = d0 ^ d1 ^ d3;
    const bool p3 = d0 ^ d2 ^ d3;

    // Full 8-bit: d0 d1 d2 d3 p0 p1 p2 p3, then right-shift by (4-cr)
    const uint8_t full = static_cast<uint8_t>(
        (d0 << 7) | (d1 << 6) | (d2 << 5) | (d3 << 4) |
        (p0 << 3) | (p1 << 2) | (p2 << 1) | p3);
    return static_cast<uint8_t>(full >> (4 - cr));
}

// ---------- Interleave one block ----------
// Takes sf_app codewords (each with cw_len bits), produces cw_len symbols.
std::vector<uint16_t> interleave_block(const std::vector<uint8_t>& codewords,
                                       int sf, int sf_app, int cw_len, bool /*ldro*/)
{
    // Build codeword bit matrix
    std::vector<std::vector<bool>> cw_bin(sf_app);
    for (int i = 0; i < sf_app; ++i) {
        if (i < static_cast<int>(codewords.size()))
            cw_bin[i] = uint_to_bits(codewords[i], cw_len);
        else
            cw_bin[i] = std::vector<bool>(cw_len, false);
    }

    // Interleave: produces sf_app bits per symbol (same rotation as decoder)
    std::vector<std::vector<bool>> inter_bin(cw_len, std::vector<bool>(sf_app, false));
    for (int i = 0; i < cw_len; ++i) {
        for (int j = 0; j < sf_app; ++j) {
            inter_bin[i][j] = cw_bin[modulo(i - j - 1, sf_app)][i];
        }
    }

    // Convert to symbol values: exact inverse of decoder's deinterleave
    // Decoder does: symbol → (-1) → (>>shift) → gray_encode → sf_app bits
    // Encoder does: sf_app bits → gray_decode → (<<shift) → (+1) → symbol
    const int mask_full = (1 << sf) - 1;
    const int mask_app = (1 << sf_app) - 1;
    std::vector<uint16_t> symbols(cw_len);
    for (int i = 0; i < cw_len; ++i) {
        uint16_t value = bits_to_uint(inter_bin[i]) & static_cast<uint16_t>(mask_app);
        uint16_t decoded = host_sim::gray_decode(value);
        if (sf_app < sf) {
            decoded = static_cast<uint16_t>((decoded << (sf - sf_app)) & mask_full);
        }
        decoded = static_cast<uint16_t>((decoded + 1) & mask_full);
        symbols[i] = decoded;
    }

    return symbols;
}

// ---------- Full payload encode ----------
// Takes raw payload bytes, returns all data symbols (header + payload).
//
// The LoRa frame encodes a single nibble stream:
//   [5 header nibbles] + [data nibbles from whitened payload+CRC]
//
// The first interleave block ("header block") consumes (sf-2) nibbles
// from this stream, always encoded at CR=4 (Hamming(8,4)), with
// sf_app = sf-2 and cw_len = 8.
//   - Positions 0..4: the 5 header field nibbles
//   - Positions 5..sf-3: the first (sf-7) data nibbles (only when sf>7)
//
// Subsequent payload blocks each consume sf_app nibbles at the
// packet's actual CR, where sf_app = sf (no LDRO) or sf-2 (LDRO).
std::vector<uint16_t> encode_packet_symbols(
    int sf, int cr, bool has_crc, bool ldro, bool implicit_header,
    const std::vector<uint8_t>& payload)
{
    // 1. CRC on raw payload; whiten only the payload bytes.
    //    CRC bytes are NOT whitened (matching GNU Radio behavior).
    //    compute_lora_crc() = CRC16(payload[0..n-3]) XOR (payload[n-2]<<8 | payload[n-1]).
    host_sim::WhiteningSequencer ws;
    auto whitened = ws.apply(payload);
    std::vector<uint8_t> data_stream = whitened;
    if (has_crc) {
        uint16_t crc_val = host_sim::lora_replay::compute_lora_crc(payload);
        data_stream.push_back(static_cast<uint8_t>(crc_val & 0xFF));
        data_stream.push_back(static_cast<uint8_t>((crc_val >> 8) & 0xFF));
    }

    // 2. Split data into nibbles (low nibble first for each byte)
    std::vector<uint8_t> data_nibbles;
    for (uint8_t byte : data_stream) {
        data_nibbles.push_back(static_cast<uint8_t>(byte & 0xF));
        data_nibbles.push_back(static_cast<uint8_t>((byte >> 4) & 0xF));
    }

    // 3. Build the merged nibble stream for the header block.
    //    Explicit: 5 header nibbles + first max(0, sf-7) data nibbles
    //    Implicit: all sf-2 nibbles are data (no header field)
    const int sf_app_hdr = sf - 2;
    int data_in_header;
    std::vector<uint8_t> header_block_nibbles;
    if (implicit_header) {
        data_in_header = sf_app_hdr;
        for (int i = 0; i < data_in_header && i < static_cast<int>(data_nibbles.size()); ++i) {
            header_block_nibbles.push_back(data_nibbles[i]);
        }
    } else {
        auto header_nibbles = host_sim::lora_replay::build_header_nibbles(
            static_cast<int>(payload.size()), has_crc, cr);
        data_in_header = std::max(0, sf_app_hdr - 5);
        header_block_nibbles = header_nibbles;
        for (int i = 0; i < data_in_header && i < static_cast<int>(data_nibbles.size()); ++i) {
            header_block_nibbles.push_back(data_nibbles[i]);
        }
    }

    // 4. Encode header block: CR=4, sf_app=sf-2, cw_len=8
    constexpr int header_cr = 4;
    constexpr int header_cw_len = 8;
    {
        std::vector<uint8_t> header_codewords;
        for (int i = 0; i < sf_app_hdr; ++i) {
            uint8_t nib = (i < static_cast<int>(header_block_nibbles.size()))
                          ? header_block_nibbles[i] : uint8_t{0};
            header_codewords.push_back(hamming_encode(nib, header_cr));
        }
        auto header_symbols = interleave_block(header_codewords, sf, sf_app_hdr,
                                               header_cw_len, false);
        // Note: header block uses sf_app < sf, so interleave_block adds parity bit

        // 5. Encode payload blocks from remaining data nibbles
        const int payload_cr = cr;
        const int payload_cw_len = payload_cr + 4;
        const int payload_sf_app = ldro ? sf - 2 : sf;

        std::vector<uint16_t> payload_symbols;
        std::size_t idx = static_cast<std::size_t>(data_in_header);
        while (idx < data_nibbles.size()) {
            std::vector<uint8_t> block_codewords;
            for (int i = 0; i < payload_sf_app && idx < data_nibbles.size(); ++i, ++idx) {
                block_codewords.push_back(hamming_encode(data_nibbles[idx], payload_cr));
            }
            auto block_syms = interleave_block(block_codewords, sf, payload_sf_app,
                                               payload_cw_len, ldro);
            payload_symbols.insert(payload_symbols.end(),
                                   block_syms.begin(), block_syms.end());
        }

        // 6. Concatenate: header + payload
        std::vector<uint16_t> all_symbols;
        all_symbols.insert(all_symbols.end(),
                           header_symbols.begin(), header_symbols.end());
        all_symbols.insert(all_symbols.end(),
                           payload_symbols.begin(), payload_symbols.end());
        return all_symbols;
    }
}

// ---------- IQ modulation ----------
std::vector<std::complex<float>> modulate_packet(
    int sf, int os_factor, int sync_word,
    int preamble_len,
    const std::vector<uint16_t>& data_symbols)
{
    const int sps = (1 << sf) * os_factor;
    std::vector<std::complex<float>> iq;

    // Leading silence so the burst detector has a noise floor.
    // The detector estimates noise from the lowest-power quartile of
    // sps-sized windows, so silence must exceed 25% of total length.
    // Use max(preamble + 12, data/2) symbols to guarantee enough margin.
    const int data_sym_count = static_cast<int>(data_symbols.size());
    const int signal_syms = preamble_len + 2 + 3 + data_sym_count; // preamble+sync+SFD+data
    const int pad_syms = std::max(preamble_len + 12, signal_syms / 2);
    const auto pad_len = static_cast<std::size_t>(sps) * static_cast<std::size_t>(pad_syms);
    iq.resize(pad_len, {0.0f, 0.0f});

    auto base_chirps = host_sim::build_chirps(sf, os_factor);

    // Preamble: preamble_len unmodulated upchirps
    for (int i = 0; i < preamble_len; ++i) {
        iq.insert(iq.end(), base_chirps.upchirp.begin(), base_chirps.upchirp.end());
    }

    // Sync word: 2 modulated upchirps
    const uint16_t sw0 = static_cast<uint16_t>(((sync_word & 0xF0) >> 4) << 3);
    const uint16_t sw1 = static_cast<uint16_t>((sync_word & 0x0F) << 3);
    auto sw0_chirp = host_sim::build_chirps_with_id(sf, os_factor, sw0);
    auto sw1_chirp = host_sim::build_chirps_with_id(sf, os_factor, sw1);
    iq.insert(iq.end(), sw0_chirp.upchirp.begin(), sw0_chirp.upchirp.end());
    iq.insert(iq.end(), sw1_chirp.upchirp.begin(), sw1_chirp.upchirp.end());

    // SFD: 2.25 downchirps
    iq.insert(iq.end(), base_chirps.downchirp.begin(), base_chirps.downchirp.end());
    iq.insert(iq.end(), base_chirps.downchirp.begin(), base_chirps.downchirp.end());
    // Quarter downchirp
    iq.insert(iq.end(), base_chirps.downchirp.begin(),
              base_chirps.downchirp.begin() + sps / 4);

    // Data symbols: modulated upchirps
    for (uint16_t sym : data_symbols) {
        auto sym_chirp = host_sim::build_chirps_with_id(sf, os_factor, sym);
        iq.insert(iq.end(), sym_chirp.upchirp.begin(), sym_chirp.upchirp.end());
    }

    // Trailing silence
    iq.resize(iq.size() + pad_len, {0.0f, 0.0f});

    return iq;
}

// ---------- CLI ----------
struct TxOptions
{
    int sf{7};
    int cr{1};  // 1..4
    int bw{125000};
    int sample_rate{0}; // default: bw
    int preamble_len{8};
    int sync_word{0x12};
    bool has_crc{true};
    bool implicit_header{false};
    bool ldro_auto{true};
    bool ldro{false};
    float snr_db{NAN};    // NaN = no noise
    float cfo_hz{0.0f};   // carrier frequency offset
    float sfo_ppm{0.0f};  // sampling frequency offset
    unsigned seed{0};     // RNG seed (0 = random)
    std::string payload;
    std::filesystem::path output;
};

void print_usage(const char* prog)
{
    std::cerr
        << "Usage: " << prog << " [options] --payload <text> --output <file.cf32>\n"
        << "  --sf <7-12>          Spreading factor (default: 7)\n"
        << "  --cr <1-4>           Coding rate 4/(4+cr) (default: 1)\n"
        << "  --bw <Hz>            Bandwidth (default: 125000)\n"
        << "  --sample-rate <Hz>   Sample rate (default: bw)\n"
        << "  --preamble <n>       Preamble length (default: 8)\n"
        << "  --sync-word <hex>    Sync word (default: 0x12)\n"
        << "  --no-crc             Disable CRC\n"
        << "  --implicit           Implicit header mode\n"
        << "  --ldro               Force LDRO on\n"
        << "  --no-ldro            Force LDRO off\n"
        << "  --snr <dB>           Add AWGN at given SNR (default: off)\n"
        << "  --cfo <Hz>           Carrier frequency offset (default: 0)\n"
        << "  --sfo <ppm>          Sampling frequency offset (default: 0)\n"
        << "  --seed <n>           RNG seed for AWGN (default: random)\n"
        << "  --payload <text>     Payload string\n"
        << "  --payload-hex <hex>  Payload as hex bytes\n"
        << "  --output <file>      Output IQ file (.cf32)\n";
}

std::optional<TxOptions> parse_args(int argc, char* argv[])
{
    TxOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << "\n";
                return {};
            }
            return argv[++i];
        };
        if (arg == "--sf") { opts.sf = std::stoi(next()); }
        else if (arg == "--cr") { opts.cr = std::stoi(next()); }
        else if (arg == "--bw") { opts.bw = std::stoi(next()); }
        else if (arg == "--sample-rate") { opts.sample_rate = std::stoi(next()); }
        else if (arg == "--preamble") { opts.preamble_len = std::stoi(next()); }
        else if (arg == "--sync-word") { opts.sync_word = std::stoi(next(), nullptr, 16); }
        else if (arg == "--no-crc") { opts.has_crc = false; }
        else if (arg == "--implicit") { opts.implicit_header = true; }
        else if (arg == "--ldro") { opts.ldro_auto = false; opts.ldro = true; }
        else if (arg == "--no-ldro") { opts.ldro_auto = false; opts.ldro = false; }
        else if (arg == "--payload") { opts.payload = next(); }
        else if (arg == "--payload-hex") {
            std::string hex = next();
            opts.payload.clear();
            for (std::size_t j = 0; j + 1 < hex.size(); j += 2) {
                opts.payload.push_back(static_cast<char>(
                    std::stoi(hex.substr(j, 2), nullptr, 16)));
            }
        }
        else if (arg == "--snr") { opts.snr_db = std::stof(next()); }
        else if (arg == "--cfo") { opts.cfo_hz = std::stof(next()); }
        else if (arg == "--sfo") { opts.sfo_ppm = std::stof(next()); }
        else if (arg == "--seed") { opts.seed = static_cast<unsigned>(std::stoul(next())); }
        else if (arg == "--output") { opts.output = next(); }
        else if (arg == "--help" || arg == "-h") { return std::nullopt; }
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            return std::nullopt;
        }
    }

    if (opts.payload.empty() || opts.output.empty()) {
        std::cerr << "Error: --payload and --output are required\n";
        return std::nullopt;
    }
    if (opts.sf < 6 || opts.sf > 12) {
        std::cerr << "Error: SF must be 6-12\n";
        return std::nullopt;
    }
    if (opts.cr < 1 || opts.cr > 4) {
        std::cerr << "Error: CR must be 1-4\n";
        return std::nullopt;
    }
    if (opts.sample_rate == 0) {
        opts.sample_rate = opts.bw;
    }
    if (opts.sample_rate < opts.bw || opts.sample_rate % opts.bw != 0) {
        std::cerr << "Error: sample_rate must be a multiple of bw\n";
        return std::nullopt;
    }
    if (opts.ldro_auto) {
        const float symbol_duration_ms =
            static_cast<float>(1 << opts.sf) * 1000.0f / static_cast<float>(opts.bw);
        opts.ldro = symbol_duration_ms > 16.0f;
    }
    return opts;
}

} // namespace

int main(int argc, char* argv[])
{
    auto opts_opt = parse_args(argc, argv);
    if (!opts_opt) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    const auto& opts = *opts_opt;

    std::vector<uint8_t> payload_bytes(opts.payload.begin(), opts.payload.end());

    std::cout << "LoRa TX Encoder\n"
              << "  SF=" << opts.sf << " CR=4/" << (4 + opts.cr)
              << " BW=" << opts.bw << " Fs=" << opts.sample_rate
              << " LDRO=" << (opts.ldro ? "on" : "off") << "\n"
              << "  Preamble=" << opts.preamble_len
              << " SyncWord=0x" << std::hex << opts.sync_word << std::dec
              << " CRC=" << (opts.has_crc ? "on" : "off")
              << " HDR=" << (opts.implicit_header ? "implicit" : "explicit")
              << "\n"
              << "  Payload (" << payload_bytes.size() << " bytes): ";
    for (uint8_t b : payload_bytes) {
        if (std::isprint(b)) std::cout << static_cast<char>(b);
        else std::cout << '.';
    }
    std::cout << "\n";

    // Encode
    auto data_symbols = encode_packet_symbols(
        opts.sf, opts.cr, opts.has_crc, opts.ldro, opts.implicit_header,
        payload_bytes);

    std::cout << "  Data symbols: " << data_symbols.size() << "\n";

    // Modulate
    const int os_factor = opts.sample_rate / opts.bw;
    auto iq = modulate_packet(opts.sf, os_factor, opts.sync_word,
                              opts.preamble_len, data_symbols);

    std::cout << "  IQ samples: " << iq.size() << "\n";

    // --- Apply channel impairments ---

    // CFO: apply progressive phase rotation  e^(j*2π*cfo*n/Fs)
    if (opts.cfo_hz != 0.0f) {
        const double phase_per_sample =
            2.0 * M_PI * static_cast<double>(opts.cfo_hz) /
            static_cast<double>(opts.sample_rate);
        double phi = 0.0;
        for (std::size_t n = 0; n < iq.size(); ++n) {
            iq[n] *= std::complex<float>(static_cast<float>(std::cos(phi)),
                                         static_cast<float>(std::sin(phi)));
            phi += phase_per_sample;
            // Keep phase in [-π, π] to maintain precision
            if (phi > M_PI) phi -= 2.0 * M_PI;
            else if (phi < -M_PI) phi += 2.0 * M_PI;
        }
        std::cout << "  CFO applied: " << opts.cfo_hz << " Hz\n";
    }

    // SFO: resample via linear interpolation at slightly different rate
    if (opts.sfo_ppm != 0.0f) {
        const double rate_ratio = 1.0 + static_cast<double>(opts.sfo_ppm) * 1e-6;
        const auto new_len = static_cast<std::size_t>(
            static_cast<double>(iq.size()) / rate_ratio);
        std::vector<std::complex<float>> resampled(new_len);
        for (std::size_t i = 0; i < new_len; ++i) {
            const double src_idx = static_cast<double>(i) * rate_ratio;
            const auto idx0 = static_cast<std::size_t>(src_idx);
            const float frac = static_cast<float>(src_idx - static_cast<double>(idx0));
            if (idx0 + 1 < iq.size()) {
                resampled[i] = iq[idx0] * (1.0f - frac) + iq[idx0 + 1] * frac;
            } else if (idx0 < iq.size()) {
                resampled[i] = iq[idx0];
            }
        }
        iq = std::move(resampled);
        std::cout << "  SFO applied: " << opts.sfo_ppm << " ppm ("
                  << iq.size() << " samples after resample)\n";
    }

    // AWGN: add complex Gaussian noise at specified SNR
    if (!std::isnan(opts.snr_db)) {
        // Measure signal power over the non-silence portion
        // (find first and last non-zero sample)
        double signal_power = 0.0;
        std::size_t sig_count = 0;
        for (const auto& s : iq) {
            const float pwr = s.real() * s.real() + s.imag() * s.imag();
            if (pwr > 1e-10f) {
                signal_power += static_cast<double>(pwr);
                ++sig_count;
            }
        }
        if (sig_count > 0) {
            signal_power /= static_cast<double>(sig_count);
        } else {
            signal_power = 1.0;
        }

        const double noise_power =
            signal_power / std::pow(10.0, static_cast<double>(opts.snr_db) / 10.0);
        const auto noise_std = static_cast<float>(std::sqrt(noise_power / 2.0));

        std::mt19937 rng(opts.seed != 0 ? opts.seed : std::random_device{}());
        std::normal_distribution<float> dist(0.0f, noise_std);
        // Only add noise to the signal portion (non-zero samples) so
        // the receiver's energy-based burst detector can still find the
        // packet.  The leading/trailing silence stays clean.
        for (auto& s : iq) {
            const float pwr = s.real() * s.real() + s.imag() * s.imag();
            if (pwr > 1e-10f) {
                s += std::complex<float>(dist(rng), dist(rng));
            }
        }
        std::cout << "  AWGN applied: SNR=" << opts.snr_db
                  << " dB (signal_pwr=" << signal_power
                  << " noise_std=" << noise_std << ")\n";
    }

    // Write output
    std::ofstream out(opts.output, std::ios::binary);
    if (!out) {
        std::cerr << "Error: cannot open " << opts.output << " for writing\n";
        return EXIT_FAILURE;
    }
    out.write(reinterpret_cast<const char*>(iq.data()),
              static_cast<std::streamsize>(iq.size() * sizeof(std::complex<float>)));
    out.close();

    std::cout << "  Written: " << opts.output << " ("
              << iq.size() * sizeof(std::complex<float>) << " bytes)\n";
    std::cout << "TX_ENCODE_OK\n";

    return EXIT_SUCCESS;
}
