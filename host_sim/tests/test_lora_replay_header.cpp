#include "host_sim/alignment.hpp"
#include "host_sim/capture.hpp"
#include "host_sim/fft_demod.hpp"
#include "host_sim/lora_params.hpp"
#include "host_sim/lora_replay/cfo_estimator.hpp"
#include "host_sim/lora_replay/header_encoder.hpp"
#include "host_sim/lora_replay/stage_processing.hpp"

#include <filesystem>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using namespace host_sim::lora_replay;

namespace
{

std::size_t compute_samples_per_symbol(const host_sim::LoRaMetadata& meta)
{
    const std::size_t chips = static_cast<std::size_t>(1) << meta.sf;
    return static_cast<std::size_t>((static_cast<long long>(meta.sample_rate) * chips) / meta.bw);
}

bool run_synthetic_header_checks()
{
    const int sfs[] = {5, 6, 7, 8, 9, 10};
    const int payload_lengths[] = {1, 5, 16, 42, 223};
    const int cr_values[] = {1, 2, 3, 4};

    for (int sf : sfs) {
        for (int cr : cr_values) {
            for (bool has_crc : {false, true}) {
                for (int len : payload_lengths) {
                    host_sim::LoRaMetadata meta{};
                    meta.sf = sf;
                    meta.bw = 125000;
                    meta.sample_rate = 500000;
                    meta.cr = cr;
                    meta.has_crc = has_crc;
                    meta.payload_len = len;
                    meta.preamble_len = 8;
                    meta.ldro = (sf >= 11);

                    std::vector<uint16_t> symbols =
                        encode_header_symbols(meta, len, has_crc, cr);
                    auto decoded = try_decode_header(symbols, 0, meta);
                    if (!decoded.success) {
                        std::cerr << "Synthetic header failed to decode for sf=" << sf
                                  << " cr=" << cr << " crc=" << has_crc
                                  << " len=" << len << "\n";
                        return false;
                    }
                    if (decoded.payload_len != len || decoded.cr != cr
                        || decoded.has_crc != has_crc) {
                        std::cerr << "Synthetic header fields mismatch sf=" << sf
                                  << " cr=" << cr << " crc=" << has_crc
                                  << " len=" << len
                                  << " -> decoded len=" << decoded.payload_len
                                  << " cr=" << decoded.cr
                                  << " crc=" << decoded.has_crc << "\n";
                        std::cerr << "Decoded nibbles:";
                        for (auto nib : decoded.nibbles) {
                            std::cerr << ' ' << static_cast<int>(nib & 0xF);
                        }
                        std::cerr << "\n";
                        return false;
                    }
                    if (decoded.checksum_field != decoded.checksum_computed) {
                        std::cerr << "Synthetic checksum mismatch\n";
                        return false;
                    }
                }
            }
        }
    }

    const std::vector<uint8_t> payload = {0x10, 0x20, 0x30, 0x40};
    auto encoded = build_payload_with_crc(payload);
    if (encoded.size() != payload.size() + 2) {
        std::cerr << "CRC builder produced unexpected length\n";
        return false;
    }
    if (compute_lora_crc(encoded) != 0) {
        std::cerr << "CRC builder did not match LoRa verification routine\n";
        return false;
    }
    return true;
}

bool run_capture_header_check()
{
    const fs::path root_dir = fs::path{PROJECT_SOURCE_DIR};
    const fs::path data_dir = root_dir / "gr_lora_sdr" / "data" / "generated";
    const fs::path capture_path = data_dir / "tx_rx_sf7_bw125000_cr1_snrm5p0.cf32";
    const fs::path metadata_path = data_dir / "tx_rx_sf7_bw125000_cr1_snrm5p0.json";

    auto samples = host_sim::load_cf32(capture_path);
    auto meta = host_sim::load_metadata(metadata_path);

    host_sim::FftDemodulator demod(meta.sf, meta.sample_rate, meta.bw);
    const std::size_t alignment =
        host_sim::find_symbol_alignment(samples,
                                        demod,
                                        meta.preamble_len,
                                        0,
                                        static_cast<uint8_t>(meta.sync_word));
    const auto freq_est =
        demod.estimate_frequency_offsets(samples.data() + static_cast<std::ptrdiff_t>(alignment),
                                         meta.preamble_len);
    demod.set_frequency_offsets(freq_est.cfo_frac, freq_est.cfo_int, freq_est.sfo_slope);
    demod.reset_symbol_counter();

    const std::size_t sps = compute_samples_per_symbol(meta);
    const std::size_t available =
        (samples.size() > alignment) ? (samples.size() - alignment) / sps : 0;
    if (available < static_cast<std::size_t>(meta.preamble_len + 16)) {
        std::cerr << "Not enough symbols demodulated for header test\n";
        return false;
    }

    std::vector<uint16_t> symbols;
    symbols.reserve(available);
    for (std::size_t idx = 0; idx < available; ++idx) {
        const auto sample_index = alignment + idx * sps;
        if (sample_index + sps > samples.size()) {
            break;
        }
        symbols.push_back(
            demod.demodulate(samples.data() + static_cast<std::ptrdiff_t>(sample_index)));
    }

    HeaderDecodeResult header;
    bool found = false;
    const std::size_t search_limit =
        std::min<std::size_t>(symbols.size(), static_cast<std::size_t>(256));
    for (std::size_t start = 0; start < search_limit; ++start) {
        if (start + 8 > symbols.size()) {
            break;
        }
        auto candidate = try_decode_header(symbols, start, meta);
        // Look for header that matches metadata AND has valid checksum
        if (candidate.success && 
            candidate.payload_len == meta.payload_len &&
            candidate.cr == meta.cr &&
            candidate.has_crc == meta.has_crc) {
            header = std::move(candidate);
            found = true;
            break;
        }
    }

    if (!found) {
        std::cerr << "Unable to decode LoRa header from reference capture\n";
        return false;
    }

    const int expected_payload_len = meta.payload_len;
    const int expected_cr = meta.cr;
    const bool expected_crc = meta.has_crc;

    if (header.payload_len != expected_payload_len) {
        std::cerr << "Payload length mismatch: got " << header.payload_len
                  << " expected " << expected_payload_len << "\n";
        return false;
    }

    if (header.cr != expected_cr) {
        std::cerr << "Code-rate mismatch: got " << header.cr
                  << " expected " << expected_cr << "\n";
        return false;
    }

    if (header.has_crc != expected_crc) {
        std::cerr << "CRC flag mismatch\n";
        return false;
    }

    if (header.checksum_field != header.checksum_computed) {
        std::cerr << "Header checksum mismatch: field=" << header.checksum_field
                  << " computed=" << header.checksum_computed << "\n";
        return false;
    }

    if (header.nibbles.size() < 5) {
        std::cerr << "Header nibble count too small: " << header.nibbles.size() << "\n";
        return false;
    }

    const int high_len = (expected_payload_len >> 4) & 0xF;
    const int low_len = expected_payload_len & 0xF;
    if ((header.nibbles[0] & 0xF) != high_len || (header.nibbles[1] & 0xF) != low_len) {
        std::cerr << "Header nibble ordering does not match spec\n";
        return false;
    }

    const bool crc_flag = (header.nibbles[2] & 0x1) != 0;
    const int cr_field = (header.nibbles[2] >> 1) & 0x7;
    if (crc_flag != header.has_crc || cr_field != header.cr) {
        std::cerr << "Header nibble values inconsistent with decoded fields\n";
        return false;
    }

    return true;
}

} // namespace

int main()
{
    if (!run_synthetic_header_checks()) {
        return 1;
    }
    if (!run_capture_header_check()) {
        return 1;
    }
    return 0;
}
