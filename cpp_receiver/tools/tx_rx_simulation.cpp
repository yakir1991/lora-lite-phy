#include "receiver.hpp"
#include "frame_sync.hpp"
#include "header_decoder.hpp"
#include "chirp_generator.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct TxConfig {
    int sf = 8;
    int cr = 4;
    int bandwidth_hz = 125000;
    int sample_rate_hz = 125000;
    bool has_crc = true;
    bool implicit_header = false;
    int preamble_len = 8;
    unsigned sync_word = 0x12u;
};

constexpr std::array<uint8_t, 255> kWhiteningSeq = {
    0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE1, 0xC2, 0x85, 0x0B, 0x17, 0x2F, 0x5E, 0xBC, 0x78, 0xF1, 0xE3,
    0xC6, 0x8D, 0x1A, 0x34, 0x68, 0xD0, 0xA0, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x11, 0x23, 0x47,
    0x8E, 0x1C, 0x38, 0x71, 0xE2, 0xC4, 0x89, 0x12, 0x25, 0x4B, 0x97, 0x2E, 0x5C, 0xB8, 0x70, 0xE0,
    0xC0, 0x81, 0x03, 0x06, 0x0C, 0x19, 0x32, 0x64, 0xC9, 0x92, 0x24, 0x49, 0x93, 0x26, 0x4D, 0x9B,
    0x37, 0x6E, 0xDC, 0xB9, 0x72, 0xE4, 0xC8, 0x90, 0x20, 0x41, 0x82, 0x05, 0x0A, 0x15, 0x2B, 0x56,
    0xAD, 0x5B, 0xB6, 0x6D, 0xDA, 0xB5, 0x6B, 0xD6, 0xAC, 0x59, 0xB2, 0x65, 0xCB, 0x96, 0x2C, 0x58,
    0xB0, 0x61, 0xC3, 0x87, 0x0F, 0x1F, 0x3E, 0x7D, 0xFB, 0xF6, 0xED, 0xDB, 0xB7, 0x6F, 0xDE, 0xBD,
    0x7A, 0xF5, 0xEB, 0xD7, 0xAE, 0x5D, 0xBA, 0x74, 0xE8, 0xD1, 0xA2, 0x44, 0x88, 0x10, 0x21, 0x43,
    0x86, 0x0D, 0x1B, 0x36, 0x6C, 0xD8, 0xB1, 0x63, 0xC7, 0x8F, 0x1E, 0x3C, 0x79, 0xF3, 0xE7, 0xCE,
    0x9C, 0x39, 0x73, 0xE6, 0xCC, 0x98, 0x31, 0x62, 0xC5, 0x8B, 0x16, 0x2D, 0x5A, 0xB4, 0x69, 0xD2,
    0xA4, 0x48, 0x91, 0x22, 0x45, 0x8A, 0x14, 0x29, 0x52, 0xA5, 0x4A, 0x95, 0x2A, 0x54, 0xA9, 0x53,
    0xA7, 0x4E, 0x9D, 0x3B, 0x77, 0xEE, 0xDD, 0xBB, 0x76, 0xEC, 0xD9, 0xB3, 0x67, 0xCF, 0x9E, 0x3D,
    0x7B, 0xF7, 0xEF, 0xDF, 0xBF, 0x7E, 0xFD, 0xFA, 0xF4, 0xE9, 0xD3, 0xA6, 0x4C, 0x99, 0x33, 0x66,
    0xCD, 0x9A, 0x35, 0x6A, 0xD4, 0xA8, 0x51, 0xA3, 0x46, 0x8C, 0x18, 0x30, 0x60, 0xC1, 0x83, 0x07,
    0x0E, 0x1D, 0x3A, 0x75, 0xEA, 0xD5, 0xAA, 0x55, 0xAB, 0x57, 0xAF, 0x5F, 0xBE, 0x7C, 0xF9, 0xF2,
    0xE5, 0xCA, 0x94, 0x28, 0x50, 0xA1, 0x42, 0x84, 0x09, 0x13, 0x27, 0x4F, 0x9F, 0x3F, 0x7F
};

int mod(int a, int b) {
    return (a % b + b) % b;
}

std::vector<int> int_to_bits(uint32_t value, int n_bits) {
    std::vector<int> bits(static_cast<std::size_t>(n_bits), 0);
    for (int i = 0; i < n_bits; ++i) {
        bits[n_bits - 1 - i] = (value >> i) & 1u;
    }
    return bits;
}

uint32_t bits_to_uint(const std::vector<int> &bits) {
    uint32_t value = 0;
    for (int bit : bits) {
        value = static_cast<uint32_t>((value << 1) | (bit & 1));
    }
    return value;
}

uint16_t crc16_accumulate(uint16_t crc, uint8_t byte) {
    for (int i = 0; i < 8; ++i) {
        const bool xor_bit = (((crc & 0x8000u) >> 8) ^ (byte & 0x80u)) != 0;
        crc = static_cast<uint16_t>(crc << 1);
        if (xor_bit) {
            crc ^= 0x1021u;
        }
        byte <<= 1;
    }
    return crc;
}

uint16_t compute_payload_crc(const std::vector<uint8_t> &payload) {
    if (payload.size() < 2) {
        uint16_t crc = 0;
        for (uint8_t byte : payload) {
            crc = crc16_accumulate(crc, byte);
        }
        return crc;
    }

    uint16_t crc = 0;
    const std::size_t upto = payload.size() - 2;
    for (std::size_t i = 0; i < upto; ++i) {
        crc = crc16_accumulate(crc, payload[i]);
    }
    crc ^= payload[payload.size() - 1];
    crc ^= static_cast<uint16_t>(payload[payload.size() - 2] << 8);
    return crc;
}

std::vector<uint8_t> build_header_nibbles(const TxConfig &cfg, int payload_len) {
    std::vector<uint8_t> hdr(5);
    hdr[0] = static_cast<uint8_t>((payload_len >> 4) & 0x0F);
    hdr[1] = static_cast<uint8_t>(payload_len & 0x0F);
    hdr[2] = static_cast<uint8_t>(((cfg.cr & 0x0F) << 1) | (cfg.has_crc ? 1 : 0));

    const bool c4 = ((hdr[0] & 0x8) >> 3) ^ ((hdr[0] & 0x4) >> 2) ^ ((hdr[0] & 0x2) >> 1) ^ (hdr[0] & 0x1);
    const bool c3 = ((hdr[0] & 0x8) >> 3) ^ ((hdr[1] & 0x8) >> 3) ^ ((hdr[1] & 0x4) >> 2) ^ ((hdr[1] & 0x2) >> 1) ^ (hdr[2] & 0x1);
    const bool c2 = ((hdr[0] & 0x4) >> 2) ^ ((hdr[1] & 0x8) >> 3) ^ (hdr[1] & 0x1) ^ ((hdr[2] & 0x8) >> 3) ^ ((hdr[2] & 0x2) >> 1);
    const bool c1 = ((hdr[0] & 0x2) >> 1) ^ ((hdr[1] & 0x4) >> 2) ^ (hdr[1] & 0x1) ^ ((hdr[2] & 0x4) >> 2) ^ ((hdr[2] & 0x2) >> 1) ^ (hdr[2] & 0x1);
    const bool c0 = (hdr[0] & 0x1) ^ ((hdr[1] & 0x2) >> 1) ^ ((hdr[2] & 0x8) >> 3) ^ ((hdr[2] & 0x4) >> 2) ^ ((hdr[2] & 0x2) >> 1) ^ (hdr[2] & 0x1);

    hdr[3] = static_cast<uint8_t>(c4);
    hdr[4] = static_cast<uint8_t>((c3 << 3) | (c2 << 2) | (c1 << 1) | c0);
    return hdr;
}

std::vector<uint8_t> whiten_payload(const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> nibbles;
    nibbles.reserve(payload.size() * 2);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        const uint8_t whitening = kWhiteningSeq[i % kWhiteningSeq.size()];
        const uint8_t whitened = static_cast<uint8_t>(payload[i] ^ whitening);
        nibbles.push_back(static_cast<uint8_t>(whitened & 0x0F));
        nibbles.push_back(static_cast<uint8_t>((whitened >> 4) & 0x0F));
    }
    return nibbles;
}

std::vector<uint8_t> hamming_encode(const TxConfig &cfg, const std::vector<uint8_t> &nibbles) {
    std::vector<uint8_t> codewords;
    codewords.reserve(nibbles.size());

    int nibble_count = 0;
    for (uint8_t nibble : nibbles) {
        const auto bits = int_to_bits(nibble & 0x0F, 4);
        const bool b3 = bits[3];
        const bool b2 = bits[2];
        const bool b1 = bits[1];
        const bool b0 = bits[0];
        const int cr_app = (nibble_count < cfg.sf - 2) ? 4 : cfg.cr;

        uint8_t encoded = 0;
        if (cr_app != 1) {
            const bool p0 = b3 ^ b2 ^ b1;
            const bool p1 = b2 ^ b1 ^ b0;
            const bool p2 = b3 ^ b2 ^ b0;
            const bool p3 = b3 ^ b1 ^ b0;
            const uint8_t raw = static_cast<uint8_t>((b3 << 7) | (b2 << 6) | (b1 << 5) | (b0 << 4) |
                                                     (p0 << 3) | (p1 << 2) | (p2 << 1) | p3);
            encoded = static_cast<uint8_t>(raw >> (4 - cr_app));
        } else {
            const bool p4 = b0 ^ b1 ^ b2 ^ b3;
            encoded = static_cast<uint8_t>((b3 << 4) | (b2 << 3) | (b1 << 2) | (b0 << 1) | p4);
        }

        codewords.push_back(encoded);
        ++nibble_count;
    }
    return codewords;
}

std::vector<uint32_t> interleave_codewords(const TxConfig &cfg, const std::vector<uint8_t> &codewords) {
    std::vector<uint32_t> symbols;
    std::size_t cw_index = 0;
    std::size_t cw_cnt = 0;

    while (cw_index < codewords.size()) {
        const bool reduced_rate = (cw_cnt < static_cast<std::size_t>(cfg.sf - 2)) || false;
        const int sf_app = reduced_rate ? cfg.sf - 2 : cfg.sf;
        const int cw_len = reduced_rate ? 8 : cfg.cr + 4;

        std::vector<std::vector<int>> cw_bin(sf_app, std::vector<int>(cw_len, 0));
        for (int row = 0; row < sf_app; ++row) {
            if (cw_index + static_cast<std::size_t>(row) < codewords.size()) {
                cw_bin[row] = int_to_bits(codewords[cw_index + static_cast<std::size_t>(row)], cw_len);
            } else {
                cw_bin[row] = std::vector<int>(cw_len, 0);
            }
        }

        cw_index += static_cast<std::size_t>(sf_app);
        cw_cnt += static_cast<std::size_t>(sf_app);
        const bool add_parity = (static_cast<int>(cw_cnt) == cfg.sf - 2);

        std::vector<std::vector<int>> inter_bin(cw_len, std::vector<int>(cfg.sf, 0));

        for (int col = 0; col < cw_len; ++col) {
            for (int j = 0; j < sf_app; ++j) {
                const int src_row = mod(col - j - 1, sf_app);
                inter_bin[col][j] = cw_bin[src_row][col];
            }
            if (add_parity) {
                const int parity = std::accumulate(inter_bin[col].begin(), inter_bin[col].begin() + sf_app, 0) & 1;
                inter_bin[col][sf_app] = parity;
            }
            symbols.push_back(bits_to_uint(inter_bin[col]));
        }
    }
    return symbols;
}

std::vector<uint32_t> gray_map(const TxConfig &cfg, const std::vector<uint32_t> &values) {
    std::vector<uint32_t> mapped(values.size());
    const uint32_t modulo = static_cast<uint32_t>(1u << cfg.sf);
    for (std::size_t idx = 0; idx < values.size(); ++idx) {
        uint32_t value = values[idx] & (modulo - 1u);
        uint32_t acc = value;
        for (int j = 1; j < cfg.sf; ++j) {
            acc ^= (value >> j);
        }
        acc = (acc + 1u) % modulo;
        mapped[idx] = acc;
    }
    return mapped;
}

std::vector<std::complex<float>> make_symbol(const TxConfig &cfg, uint32_t id) {
    const std::size_t os_factor = static_cast<std::size_t>(cfg.sample_rate_hz) / static_cast<std::size_t>(cfg.bandwidth_hz);
    const std::size_t chips_per_symbol = static_cast<std::size_t>(1) << cfg.sf;
    const std::size_t total_samples = chips_per_symbol * os_factor;
    std::vector<std::complex<float>> symbol(total_samples);

    const double N = static_cast<double>(chips_per_symbol);
    const double os = static_cast<double>(os_factor);
    const int fold = static_cast<int>(N * os - static_cast<double>(id) * os);

    for (std::size_t n = 0; n < total_samples; ++n) {
        const double nd = static_cast<double>(n);
        double phase;
        if (static_cast<int>(n) < fold) {
            phase = 2.0 * M_PI * ((nd * nd) / (2.0 * N * os * os) + ((static_cast<double>(id) / N) - 0.5) * (nd / os));
        } else {
            phase = 2.0 * M_PI * ((nd * nd) / (2.0 * N * os * os) + ((static_cast<double>(id) / N) - 1.5) * (nd / os));
        }
        symbol[n] = std::complex<float>(std::cos(phase), std::sin(phase));
    }
    return symbol;
}

std::vector<std::complex<float>> make_downchirp(const TxConfig &cfg) {
    const auto up = make_symbol(cfg, 0);
    std::vector<std::complex<float>> down(up.size());
    for (std::size_t i = 0; i < up.size(); ++i) {
        down[i] = std::conj(up[i]);
    }
    return down;
}

std::pair<uint32_t, uint32_t> split_sync_word(unsigned sync_word) {
    const uint32_t msb = static_cast<uint32_t>(((sync_word & 0xF0u) >> 4u) << 3u);
    const uint32_t lsb = static_cast<uint32_t>((sync_word & 0x0Fu) << 3u);
    return {msb, lsb};
}

std::vector<std::complex<float>> modulate_symbols(const TxConfig &cfg, const std::vector<uint32_t> &symbols) {
    const std::size_t os_factor = static_cast<std::size_t>(cfg.sample_rate_hz) / static_cast<std::size_t>(cfg.bandwidth_hz);
    const std::size_t chips_per_symbol = static_cast<std::size_t>(1) << cfg.sf;
    const std::size_t samples_per_symbol = chips_per_symbol * os_factor;

    std::vector<std::complex<float>> iq;
    iq.reserve((cfg.preamble_len + 5 + symbols.size()) * samples_per_symbol);

    const auto base_up = make_symbol(cfg, 0);
    const auto base_down = make_downchirp(cfg);
    const auto [sync0, sync1] = split_sync_word(cfg.sync_word);

    for (int i = 0; i < cfg.preamble_len; ++i) {
        iq.insert(iq.end(), base_up.begin(), base_up.end());
    }
    const auto sync_up0 = make_symbol(cfg, sync0);
    iq.insert(iq.end(), sync_up0.begin(), sync_up0.end());
    const auto sync_up1 = make_symbol(cfg, sync1);
    iq.insert(iq.end(), sync_up1.begin(), sync_up1.end());

    iq.insert(iq.end(), base_down.begin(), base_down.end());
    iq.insert(iq.end(), base_down.begin(), base_down.end());
    iq.insert(iq.end(), base_down.begin(), base_down.begin() + static_cast<long>(samples_per_symbol / 4));

    for (uint32_t sym : symbols) {
        const auto mod = make_symbol(cfg, sym % static_cast<uint32_t>(1u << cfg.sf));
        iq.insert(iq.end(), mod.begin(), mod.end());
    }

    return iq;
}

std::vector<std::complex<float>> build_frame_iq(const TxConfig &cfg, const std::vector<uint8_t> &payload_bytes) {
    if (cfg.implicit_header) {
        throw std::runtime_error("Implicit header mode not implemented in transmitter");
    }
    if (payload_bytes.empty()) {
        throw std::runtime_error("Payload must be non-empty");
    }

    const auto header_nibbles = build_header_nibbles(cfg, static_cast<int>(payload_bytes.size()));
    const uint16_t crc = cfg.has_crc ? compute_payload_crc(payload_bytes) : 0;
    std::vector<uint8_t> crc_nibbles;
    if (cfg.has_crc) {
        crc_nibbles = {
            static_cast<uint8_t>(crc & 0x0Fu),
            static_cast<uint8_t>((crc >> 4) & 0x0Fu),
            static_cast<uint8_t>((crc >> 8) & 0x0Fu),
            static_cast<uint8_t>((crc >> 12) & 0x0Fu)
        };
    }

    const auto payload_nibbles = whiten_payload(payload_bytes);

    std::vector<uint8_t> full_nibbles;
    full_nibbles.reserve(header_nibbles.size() + payload_nibbles.size() + crc_nibbles.size());
    full_nibbles.insert(full_nibbles.end(), header_nibbles.begin(), header_nibbles.end());
    full_nibbles.insert(full_nibbles.end(), payload_nibbles.begin(), payload_nibbles.end());
    full_nibbles.insert(full_nibbles.end(), crc_nibbles.begin(), crc_nibbles.end());

    const auto codewords = hamming_encode(cfg, full_nibbles);
    const auto interleaved = interleave_codewords(cfg, codewords);
    const auto gray = gray_map(cfg, interleaved);

    return modulate_symbols(cfg, gray);
}

std::vector<uint8_t> string_to_bytes(std::string_view text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

} // namespace

void write_cf32_file(const std::filesystem::path &path, const std::vector<std::complex<float>> &samples) {
    if (path.has_parent_path() && !path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        throw std::runtime_error("Failed to open dump path for writing: " + path.string());
    }
    for (const auto &sample : samples) {
        const float re = sample.real();
        const float im = sample.imag();
        ofs.write(reinterpret_cast<const char *>(&re), sizeof(float));
        ofs.write(reinterpret_cast<const char *>(&im), sizeof(float));
    }
}

int main(int argc, char **argv) {
    std::string message = "yo yo yo whats up";
    std::string dump_path;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [--dump=path] [--verbose] [message]\n";
            return 0;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg.rfind("--dump=", 0) == 0) {
            dump_path = arg.substr(std::string("--dump=").size());
        } else {
            message = arg;
        }
    }

    TxConfig cfg;

    const auto iq_double = build_frame_iq(cfg, string_to_bytes(message));
    std::vector<lora::IqLoader::Sample> iq(iq_double.begin(), iq_double.end());

    if (!dump_path.empty()) {
        write_cf32_file(dump_path, iq_double);
    }

    lora::DecodeParams params;
    params.sf = cfg.sf;
    params.bandwidth_hz = cfg.bandwidth_hz;
    params.sample_rate_hz = cfg.sample_rate_hz;
    params.sync_word = cfg.sync_word;
    params.ldro_enabled = false;

    lora::Receiver receiver(params);
    const auto result = receiver.decode_samples(iq);

    lora::FrameSynchronizer fs(params.sf, params.bandwidth_hz, params.sample_rate_hz);
    lora::HeaderDecoder header_decoder(params.sf, params.bandwidth_hz, params.sample_rate_hz);
    const auto sync = fs.synchronize(iq);
    if (!sync.has_value()) {
        std::cerr << "Frame synchronization failed; generated waveform is inconsistent\n";
        return EXIT_FAILURE;
    }
    const auto header = header_decoder.decode(iq, *sync);
    if (!header.has_value()) {
        std::cerr << "Header decode failed for generated waveform\n";
        return EXIT_FAILURE;
    }
    if (verbose) {
        std::cout << "Header: fcs_ok=" << (header->fcs_ok ? "yes" : "no")
                  << " len=" << header->payload_length
                  << " cr=" << header->cr
                  << " has_crc=" << (header->has_crc ? "yes" : "no") << '\n';
        std::cout << "First symbols:";
        auto span = header->raw_symbol_view;
        for (std::size_t i = 0; i < span.size(); ++i) {
            std::cout << ' ' << span[i];
        }
        std::cout << '\n';
    }

    if (!result.success) {
        std::cerr << "Decode failed (frame_synced=" << result.frame_synced
                  << ", header_ok=" << result.header_ok
                  << ", payload_crc_ok=" << result.payload_crc_ok << ")\n";
        return EXIT_FAILURE;
    }

    std::string decoded(result.payload.begin(), result.payload.end());
    std::cout << "Decoded payload: " << decoded << '\n';
    return EXIT_SUCCESS;
}
