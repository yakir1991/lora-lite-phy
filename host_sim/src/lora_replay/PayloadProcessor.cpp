#include "host_sim/lora_replay/PayloadProcessor.hpp"
#include "host_sim/whitening.hpp"
#include "host_sim/lora_replay/stage_processing.hpp"
#include "host_sim/deinterleaver.hpp"
#include "host_sim/hamming.hpp"
#include "host_sim/gray.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <optional>
#include <cstdlib>

namespace host_sim::lora_replay {

namespace {

constexpr std::size_t kExplicitHeaderNibbles = 5;

bool use_gnuradio_demod()
{
    static const bool enabled = []() {
        const char* env = std::getenv("HOST_SIM_GNURADIO_DEMOD");
        return env && *env != '\0' && *env != '0';
    }();
    return enabled;
}

} // namespace

PayloadProcessor::PayloadProcessor(const host_sim::LoRaMetadata& metadata)
    : metadata_(metadata)
{
}

PayloadResult PayloadProcessor::process(const std::vector<uint16_t>& symbols,
                                        std::size_t symbol_cursor,
                                        int payload_len,
                                        int cr,
                                        bool has_crc,
                                        const HeaderDecodeResult& header,
                                        const std::vector<uint16_t>& header_block)
{
    if (std::getenv("HOST_SIM_DEBUG_REPLAY")) {
        std::cerr << "[debug] PayloadProcessor::process called!" << std::endl;
    }
    PayloadResult result;
    
    // Process header stage outputs first
    const bool header_ldro_mode = header.ldro_enabled.value_or(metadata_.sf > 6);
    host_sim::DeinterleaverConfig header_cfg{metadata_.sf, 4, true, header_ldro_mode};
    header_cfg.apply_symbol_minus_one = true;
    const int header_stage_symbols = header_cfg.is_header ? 8 : header_cfg.cr + 4;
    const std::size_t used_symbols = std::min<std::size_t>(static_cast<std::size_t>(header_stage_symbols), header_block.size());
    std::vector<uint16_t> header_block_stage(header_block.begin(), header_block.begin() + used_symbols);
    append_fft_gray(header_block_stage, true, metadata_.ldro, metadata_.sf, result.stage_outputs);


    const int header_sf_app = std::max(metadata_.sf - 2, 0);
    if (header_stage_symbols > 0 && header_sf_app > 0 &&
        header_block_stage.size() >= static_cast<std::size_t>(header_stage_symbols)) {
        host_sim::DeinterleaverConfig stage_cfg = header_cfg;
        stage_cfg.use_iterative_gray = false;
        stage_cfg.sf_app_override = header_sf_app;
        std::size_t consumed_block = 0;
        try {
            auto stage_codewords = host_sim::deinterleave(header_block_stage, stage_cfg, consumed_block);
            for (auto cw : stage_codewords) {
                result.stage_outputs.deinterleaver.push_back(static_cast<uint16_t>(cw));
            }
            auto stage_nibbles = host_sim::hamming_decode_block(stage_codewords, true, 4);
            for (auto nib : stage_nibbles) {
                result.stage_outputs.hamming.push_back(static_cast<uint8_t>(nib & 0xF));
            }
        } catch (...) {
            // Fall back to decoded header codewords if stage-specific decode fails.
            const std::size_t stage_codewords =
                std::min<std::size_t>(header.codewords.size(),
                                      static_cast<std::size_t>(header_sf_app));
            for (std::size_t i = 0; i < stage_codewords; ++i) {
                result.stage_outputs.deinterleaver.push_back(static_cast<uint16_t>(header.codewords[i]));
            }
            for (std::size_t i = 0; i < stage_codewords && i < header.nibbles.size(); ++i) {
                result.stage_outputs.hamming.push_back(static_cast<uint8_t>(header.nibbles[i] & 0xF));
            }
        }
    } else {
        const std::size_t stage_codewords = std::min<std::size_t>(
            header.codewords.size(), static_cast<std::size_t>(std::max(metadata_.sf - 2, 0)));
        for (std::size_t i = 0; i < stage_codewords; ++i) {
            result.stage_outputs.deinterleaver.push_back(static_cast<uint16_t>(header.codewords[i]));
        }
        for (std::size_t i = 0; i < stage_codewords && i < header.nibbles.size(); ++i) {
            result.stage_outputs.hamming.push_back(static_cast<uint8_t>(header.nibbles[i] & 0xF));
        }
    }

    // Process payload
    std::size_t nibble_target = static_cast<std::size_t>(payload_len) * 2;
    if (has_crc) {
        nibble_target += 4; // 2 bytes CRC
    }

    host_sim::DeinterleaverConfig payload_cfg{metadata_.sf, cr, false, metadata_.ldro};
    // Match gr-lora_sdr: fft_demod produces the symbol domain we want, then gray_mapping applies
    // out = in ^ (in >> 1) before deinterleaving.
    payload_cfg.use_iterative_gray = false;
    payload_cfg.apply_symbol_minus_one = true;
    std::vector<uint8_t> payload_nibbles;
    const bool debug_payload_block = (std::getenv("HOST_SIM_DEBUG_PAYLOAD") != nullptr);

    const int payload_cw_len = cr + 4;
    const bool suppress_payload_stage =
        (std::getenv("HOST_SIM_SUPPRESS_PAYLOAD_STAGE") != nullptr) ||
        (use_gnuradio_demod() && metadata_.sf <= 6);
    const bool header_checksum_valid =
        (header.checksum_field >= 0 && header.checksum_field == header.checksum_computed);
    if (!metadata_.implicit_header && header_checksum_valid &&
        header.nibbles.size() > kExplicitHeaderNibbles) {
        payload_nibbles.insert(payload_nibbles.end(),
                               header.nibbles.begin() + static_cast<std::ptrdiff_t>(kExplicitHeaderNibbles),
                               header.nibbles.end());
        if (debug_payload_block) {
            std::cout << "[payload-debug] carried "
                      << (header.nibbles.size() - kExplicitHeaderNibbles)
                      << " header nibbles into payload stream\n";
        }
    }

    std::size_t current_cursor = symbol_cursor;

    // In implicit-header mode, gr-lora_sdr still marks the first 8 symbols as header-coded
    // (is_header=true, CR=4) but the decoded nibbles are payload data. The host keeps the
    // payload cursor after this 8-symbol block, so decode it from header_block.
    if (metadata_.implicit_header && payload_nibbles.size() < nibble_target) {
        const int header_block_symbols = 8;
        if (header_block.size() >= static_cast<std::size_t>(header_block_symbols)) {
            std::vector<uint16_t> first_block(header_block.begin(),
                                              header_block.begin() + header_block_symbols);

            host_sim::DeinterleaverConfig first_cfg{metadata_.sf, 4, true, metadata_.ldro};
            first_cfg.apply_symbol_minus_one = true;
            first_cfg.use_iterative_gray = false;

            std::size_t consumed_block = 0;
            auto codewords = host_sim::deinterleave(first_block, first_cfg, consumed_block);
            if (consumed_block > 0) {
                auto nibs = host_sim::hamming_decode_block(codewords, true, 4);
                payload_nibbles.insert(payload_nibbles.end(), nibs.begin(), nibs.end());
            }
        }
    }

    while (current_cursor + payload_cw_len <= symbols.size() && payload_nibbles.size() < nibble_target) {
        std::vector<uint16_t> block(symbols.begin() + current_cursor, symbols.begin() + current_cursor + payload_cw_len);
        
        if (debug_payload_block && payload_nibbles.empty()) {
            std::cout << "[payload-debug] CR=" << static_cast<int>(cr) << '\n';
            std::cout << "[payload-debug] first block raw: ";
            for (auto s : block) {
                std::cout << s << ' ';
            }
            std::cout << "\n[payload-debug] first block gray-decode: ";
            for (auto s : block) {
                std::cout << host_sim::gray_decode(s) << ' ';
            }
            std::cout << '\n';
        }

        std::size_t consumed_block = 0;
        auto codewords = host_sim::deinterleave(block, payload_cfg, consumed_block);
        if (consumed_block == 0) break;
        
        if (debug_payload_block && payload_nibbles.empty()) {
            std::cout << "[payload-debug] first block codewords: ";
            for (auto c : codewords) {
                std::cout << static_cast<int>(c) << ' ';
            }
            std::cout << '\n';

            auto nibs_debug = host_sim::hamming_decode_block(codewords, false, cr);
            std::cout << "[payload-debug] first block nibbles: ";
            for (auto n : nibs_debug) {
                std::cout << static_cast<int>(n) << ' ';
            }
            std::cout << '\n';
        }

        current_cursor += consumed_block;
        if (!suppress_payload_stage) {
            append_fft_gray(block, false, metadata_.ldro, metadata_.sf, result.stage_outputs);
        }
        auto nibs = host_sim::hamming_decode_block(codewords, false, cr);

        payload_nibbles.insert(payload_nibbles.end(), nibs.begin(), nibs.end());
        if (!suppress_payload_stage) {
            for (auto cw : codewords) {
                result.stage_outputs.deinterleaver.push_back(static_cast<uint16_t>(cw));
            }
            for (auto nib : nibs) {
                result.stage_outputs.hamming.push_back(static_cast<uint8_t>(nib & 0xF));
            }
        }
    }

    if (debug_payload_block) {
        std::cout << "[payload-debug] total payload nibbles: " << payload_nibbles.size() << '\n';
        const std::size_t nibble_dump = std::min<std::size_t>(payload_nibbles.size(), 32);
        std::cout << "[payload-debug] nibble stream: ";
        for (std::size_t i = 0; i < nibble_dump; ++i) {
            std::cout << static_cast<int>(payload_nibbles[i] & 0xF) << ' ';
        }
        if (payload_nibbles.size() > nibble_dump) {
            std::cout << "...";
        }
        std::cout << '\n';
    }

    result.payload_bytes.reserve(payload_nibbles.size() / 2);
    for (std::size_t i = 0; i + 1 < payload_nibbles.size(); i += 2) {
        // LoRa spec: low nibble is first, high nibble is second
        const uint8_t lsb_nibble = static_cast<uint8_t>(payload_nibbles[i] & 0xF);
        const uint8_t msb_nibble = static_cast<uint8_t>(payload_nibbles[i + 1] & 0xF);
        const uint8_t byte = static_cast<uint8_t>((msb_nibble << 4) | lsb_nibble);
        
        result.payload_bytes.push_back(byte);
    }

    if (debug_payload_block) {
        std::cout << "[payload-debug] bytes before whitening: ";
        for (auto b : result.payload_bytes) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b) << ' ';
        }
        std::cout << std::dec << '\n';
    }
    if (result.payload_bytes.size() > payload_len + (has_crc ? 2 : 0)) {
        result.payload_bytes.resize(payload_len + (has_crc ? 2 : 0));
    }

    host_sim::WhiteningSequencer seq;
    if (has_crc && result.payload_bytes.size() >= static_cast<std::size_t>(payload_len + 2)) {
        // Match gr_lora_sdr dewhitening: the CRC bytes are *not* dewhitened.
        const std::vector<uint8_t> whitened_payload_only(result.payload_bytes.begin(),
                                                         result.payload_bytes.begin() + payload_len);
        result.decoded_payload = seq.undo(whitened_payload_only);
        result.decoded_payload.push_back(result.payload_bytes[payload_len]);
        result.decoded_payload.push_back(result.payload_bytes[payload_len + 1]);
    } else {
        result.decoded_payload = seq.undo(result.payload_bytes);
    }

    if (debug_payload_block) {
        std::cout << "[payload-debug] decoded payload (data only): ";
        for (std::size_t i = 0; i < std::min<std::size_t>(result.decoded_payload.size(), payload_len); ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(result.decoded_payload[i]) << ' ';
        }
        std::cout << std::dec << '\n';
    }

    if (has_crc) {
        if (payload_len >= 2 && result.decoded_payload.size() >= payload_len + 2) {
            std::vector<uint8_t> data_only(result.decoded_payload.begin(),
                                           result.decoded_payload.begin() + payload_len);
            const uint16_t computed_crc = compute_lora_crc(data_only);

            const uint8_t crc_lsb = result.decoded_payload[payload_len];
            const uint8_t crc_msb = result.decoded_payload[payload_len + 1];
            const uint16_t decoded_crc = (static_cast<uint16_t>(crc_msb) << 8)
                                         | static_cast<uint16_t>(crc_lsb);

            result.computed_crc = computed_crc;
            result.decoded_crc = decoded_crc;
            result.crc_ok = (computed_crc == decoded_crc);

            if (!result.crc_ok && payload_len >= 2) {
                // Legacy gr_lora_sdr check: CRC bytes embedded inside the payload_len window
                const std::size_t legacy_len = std::min<std::size_t>(payload_len, result.decoded_payload.size());
                std::vector<uint8_t> legacy_payload(result.decoded_payload.begin(),
                                                    result.decoded_payload.begin() + legacy_len);
                uint16_t legacy_syndrome = compute_lora_crc_syndrome(legacy_payload);
                if (legacy_syndrome == 0 && legacy_len >= 2) {
                    std::cout << "[payload] CRC passed with legacy gr_lora_sdr logic (embedded CRC)" << std::endl;
                    result.crc_ok = true;
                    const uint8_t l_crc_lsb = legacy_payload[legacy_len - 2];
                    const uint8_t l_crc_msb = legacy_payload[legacy_len - 1];
                    result.decoded_crc = static_cast<uint16_t>(l_crc_msb) << 8 | static_cast<uint16_t>(l_crc_lsb);
                    result.computed_crc = result.decoded_crc;
                }
            }
        } else {
            result.crc_ok = false;
            std::cout << "[payload] insufficient bytes to validate CRC (len=" << payload_len
                      << ", decoded=" << result.decoded_payload.size() << ")" << std::endl;
            // Avoid claiming CRC=0 when we simply didn't decode enough bytes.
            result.decoded_crc = 0;
            result.computed_crc = 0;
        }
    } else {
        result.crc_ok = true; // No CRC to check
    }

    return result;
}

} // namespace host_sim::lora_replay
