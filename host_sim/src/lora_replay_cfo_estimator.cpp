#include "host_sim/lora_replay/cfo_estimator.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <vector>

namespace host_sim::lora_replay
{

namespace
{

HeaderDecodeResult decode_header_block(const std::vector<uint16_t>& symbols,
                                       std::size_t start,
                                       const host_sim::LoRaMetadata& meta,
                                       bool force_header_no_ldro,
                                       bool debug_header,
                                       bool use_legacy_gray)
{
    auto compute_header_checksum = [](int payload_len, bool has_crc, int cr) {
        bool h[12] = {
            static_cast<bool>((payload_len >> 7) & 0x1),
            static_cast<bool>((payload_len >> 6) & 0x1),
            static_cast<bool>((payload_len >> 5) & 0x1),
            static_cast<bool>((payload_len >> 4) & 0x1),
            static_cast<bool>((payload_len >> 3) & 0x1),
            static_cast<bool>((payload_len >> 2) & 0x1),
            static_cast<bool>((payload_len >> 1) & 0x1),
            static_cast<bool>(payload_len & 0x1),
            has_crc,
            static_cast<bool>((cr >> 2) & 0x1),
            static_cast<bool>((cr >> 1) & 0x1),
            static_cast<bool>(cr & 0x1),
        };

        const int G[5][12] = {
            {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
            {1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0},
            {0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1},
            {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1},
            {0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1},
        };

        int bits[5] = {0, 0, 0, 0, 0};
        for (int row = 0; row < 5; ++row) {
            int acc = 0;
            for (int col = 0; col < 12; ++col) {
                acc ^= (G[row][col] & 0x1) & static_cast<int>(h[col]);
            }
            bits[row] = acc & 0x1;
        }
        return (bits[0] << 4) | (bits[1] << 3) | (bits[2] << 2) | (bits[3] << 1) | bits[4];
    };

    const bool header_auto_ldro = (meta.sf > 6);
    const bool effective_ldro = header_auto_ldro && !force_header_no_ldro;

    HeaderDecodeResult result;
    result.ldro_enabled = effective_ldro;
    if (start + 8 > symbols.size()) {
        return result;
    }

        host_sim::DeinterleaverConfig header_cfg{meta.sf, 4, true, meta.ldro};
        header_cfg.apply_symbol_minus_one = true;
    header_cfg.header_force_no_ldro = force_header_no_ldro;
    header_cfg.use_iterative_gray = !use_legacy_gray;
    header_cfg.sf_app_override = 0;
    const int block_symbols = 8;
    std::size_t cursor = start;
    std::size_t total_consumed = 0;
    std::vector<uint8_t> header_nibbles;
    std::vector<uint16_t> header_codewords;
    while (header_nibbles.size() < 5 && cursor + block_symbols <= symbols.size()) {
        std::vector<uint16_t> header_input(symbols.begin() + cursor,
                                           symbols.begin() + cursor + block_symbols);
        if (debug_header) {
            std::cout << "Header symbols (start=" << cursor << "):";
            for (auto value : header_input) {
                std::cout << ' ' << value;
            }
            std::cout << "\n";
        }

        std::size_t consumed_block = 0;
        auto codewords = host_sim::deinterleave(header_input, header_cfg, consumed_block);
        if (consumed_block == 0) {
            break;
        }
        total_consumed += consumed_block;
        cursor += consumed_block;
        header_codewords.insert(header_codewords.end(), codewords.begin(), codewords.end());

        if (debug_header) {
            std::cout << "Deinterleaved codewords:";
            for (auto cw : codewords) {
                std::cout << ' ' << std::hex << static_cast<int>(cw) << std::dec;
            }
            std::cout << "\n";
        }

        auto nibbles = host_sim::hamming_decode_block(codewords, true, 4);
        if (debug_header) {
            std::cout << "Header nibbles:";
            for (auto nib : nibbles) {
                std::cout << ' ' << std::hex << static_cast<int>(nib & 0xF) << std::dec;
            }
            std::cout << "\n";
        }
        header_nibbles.insert(header_nibbles.end(), nibbles.begin(), nibbles.end());
    }

    if (header_nibbles.size() < 5) {
        result.codewords = std::move(header_codewords);
        result.nibbles = header_nibbles;
        result.consumed_symbols = total_consumed;
        return result;
    }

    const int n0 = header_nibbles[0] & 0xF;
    const int n1 = header_nibbles[1] & 0xF;
    const int n2 = header_nibbles[2] & 0xF;
    const int n3 = header_nibbles[3] & 0xF;
    const int n4 = header_nibbles[4] & 0xF;

    const int payload_len = (n0 << 4) | n1;
    const bool has_crc = (n2 & 0x1) != 0;
    const int cr = (n2 >> 1) & 0x7;
    const int header_chk = ((n3 & 0x1) << 4) | n4;

    const int computed_checksum = compute_header_checksum(payload_len, has_crc, cr);

    result.checksum_field = header_chk;
    result.checksum_computed = computed_checksum;

    if (debug_header) {
        std::cout << "[header-debug] fields payload=" << payload_len
              << " crc_flag=" << has_crc
              << " cr=" << cr
              << " chk=" << header_chk
              << " computed=" << computed_checksum
              << " ldro=" << (effective_ldro ? "on" : "off")
                  << "\n";
    }

    if (payload_len <= 0 || header_chk != computed_checksum) {
        result.nibbles = header_nibbles;
        result.payload_len = payload_len;
        result.has_crc = has_crc;
        result.cr = cr;
        return result;
    }

    result.success = true;
    result.payload_len = payload_len;
    result.has_crc = has_crc;
    result.cr = cr;
    result.checksum_field = header_chk;
    result.checksum_computed = computed_checksum;
    result.consumed_symbols = total_consumed;
    result.codewords = std::move(header_codewords);
    result.nibbles = std::move(header_nibbles);
    return result;
}

} // namespace

HeaderDecodeResult try_decode_header(const std::vector<uint16_t>& symbols,
                                     std::size_t start,
                                     const host_sim::LoRaMetadata& meta)
{
    static const bool debug_header = (std::getenv("HOST_SIM_DEBUG_HEADER") != nullptr);
    const bool header_auto_ldro = (meta.sf > 6);
    auto decode_with = [&](bool force_header_no_ldro, bool use_legacy_gray) {
        return decode_header_block(symbols, start, meta, force_header_no_ldro, debug_header, use_legacy_gray);
    };

    auto update_best = [](HeaderDecodeResult current, const HeaderDecodeResult& candidate) {
        if (candidate.success) {
            return candidate;
        }
        if (!current.success) {
            if (candidate.nibbles.size() > current.nibbles.size()
                || candidate.consumed_symbols > current.consumed_symbols) {
                return candidate;
            }
        }
        return current;
    };

    HeaderDecodeResult best = decode_with(false, true);
    if (best.success || !header_auto_ldro) {
        if (best.success) {
            return best;
        }
    }

    if (header_auto_ldro) {
        auto ldro_off = decode_with(true, true);
        if (ldro_off.success) {
            if (debug_header) {
                std::cout << "[header] LDRO-off retry succeeded at start=" << start << "\n";
            }
            return ldro_off;
        }
        best = update_best(best, ldro_off);
    }

    auto iterative = decode_with(false, false);
    if (iterative.success) {
        return iterative;
    }
    best = update_best(best, iterative);

    if (header_auto_ldro) {
        auto iterative_ldro_off = decode_with(true, false);
        if (iterative_ldro_off.success) {
            if (debug_header) {
                std::cout << "[header] LDRO-off retry succeeded at start=" << start << " (iterative gray)\n";
            }
            return iterative_ldro_off;
        }
        best = update_best(best, iterative_ldro_off);
    }

    return best;
}

std::optional<uint16_t> detect_integer_cfo(const std::vector<uint16_t>& symbols,
                                           const std::vector<uint16_t>& reference,
                                           int sf)
{
    if (symbols.empty() || reference.size() < symbols.size() || sf <= 0) {
        return std::nullopt;
    }
    const uint16_t mask = static_cast<uint16_t>((1u << sf) - 1u);
    const std::size_t window = std::min<std::size_t>(32, symbols.size());
    std::vector<int> histogram(static_cast<std::size_t>(mask) + 1, 0);
    for (std::size_t i = 0; i < window; ++i) {
        const uint16_t delta = static_cast<uint16_t>((reference[i] - symbols[i]) & mask);
        ++histogram[delta];
    }
    int best_count = 0;
    uint16_t best_delta = 0;
    for (std::size_t delta = 0; delta < histogram.size(); ++delta) {
        if (histogram[delta] > best_count) {
            best_count = histogram[delta];
            best_delta = static_cast<uint16_t>(delta);
        }
    }
    if (best_delta != 0 && best_count >= 8 && best_count >= static_cast<int>(window / 2)) {
        return best_delta;
    }
    return std::nullopt;
}

std::optional<SyncDetectionResult> detect_sync_pair(const std::vector<uint16_t>& symbols,
                                                    const host_sim::LoRaMetadata& meta,
                                                    std::size_t max_search,
                                                    std::size_t min_zero_run)
{
    if (symbols.size() < 2 || meta.sf <= 0) {
        return std::nullopt;
    }
    const uint16_t mask = static_cast<uint16_t>((1u << meta.sf) - 1u);
    const uint16_t sync_high = static_cast<uint16_t>(((meta.sync_word >> 4) & 0x0F) * 8u) & mask;
    const uint16_t sync_low = static_cast<uint16_t>((meta.sync_word & 0x0F) * 8u) & mask;
    const std::size_t limit = std::min<std::size_t>(symbols.size() - 1, max_search);
    std::optional<SyncDetectionResult> best;
    std::size_t best_zero_run = 0;
    for (std::size_t idx = 0; idx <= limit; ++idx) {
        if (symbols[idx] == sync_high && symbols[idx + 1] == sync_low) {
            std::size_t zero_run = 0;
            std::size_t back = idx;
            while (back > 0 && symbols[back - 1] == 0) {
                ++zero_run;
                --back;
            }
            if (zero_run >= min_zero_run) {
                return SyncDetectionResult{idx, sync_high, sync_low};
            }
            if (zero_run > best_zero_run) {
                best_zero_run = zero_run;
                best = SyncDetectionResult{idx, sync_high, sync_low};
            }
        }
    }
    return best;
}

std::optional<PreambleCfoEstimate> infer_integer_cfo_from_preamble(const std::vector<uint16_t>& symbols,
                                                                   const host_sim::LoRaMetadata& meta,
                                                                   std::size_t max_window)
{
    if (symbols.empty() || meta.sf <= 0 || meta.preamble_len <= 0) {
        return std::nullopt;
    }
    const uint16_t mask = static_cast<uint16_t>((1u << meta.sf) - 1u);
    const std::size_t desired = static_cast<std::size_t>(std::max(meta.preamble_len - 1, 0));
    if (desired == 0) {
        return std::nullopt;
    }
    const std::size_t window = std::min<std::size_t>({symbols.size(), desired, max_window});
    if (window < 4) {
        return std::nullopt;
    }
    std::vector<int> histogram(static_cast<std::size_t>(mask) + 1, 0);
    for (std::size_t i = 0; i < window; ++i) {
        const uint16_t value = static_cast<uint16_t>(symbols[i] & mask);
        ++histogram[value];
    }
    int best_count = 0;
    uint16_t best_value = 0;
    for (std::size_t bin = 0; bin < histogram.size(); ++bin) {
        if (histogram[bin] > best_count) {
            best_count = histogram[bin];
            best_value = static_cast<uint16_t>(bin);
        }
    }
    if (best_count < 2) {
        return std::nullopt;
    }
    const double ratio = static_cast<double>(best_count) / static_cast<double>(window);
    if (ratio < 0.6) {
        return std::nullopt;
    }
    const uint16_t modulus = static_cast<uint16_t>(mask + 1u);
    const uint16_t offset = static_cast<uint16_t>((modulus - best_value) & mask);
    return PreambleCfoEstimate{offset, window, static_cast<std::size_t>(best_count)};
}

void apply_integer_offset(std::vector<uint16_t>& values, uint16_t offset, int sf)
{
    if (values.empty() || sf <= 0 || offset == 0) {
        return;
    }
    const uint16_t mask = static_cast<uint16_t>((1u << sf) - 1u);
    for (auto& v : values) {
        v = static_cast<uint16_t>((v + offset) & mask);
    }
}

std::optional<uint16_t> find_header_based_integer_cfo(const std::vector<uint16_t>& symbols,
                                                      const host_sim::LoRaMetadata& meta,
                                                      int search_radius)
{
    if (symbols.size() < 8 || meta.sf <= 0 || search_radius <= 0) {
        return std::nullopt;
    }
    const uint16_t mask = static_cast<uint16_t>((1u << meta.sf) - 1u);
    const int modulus = static_cast<int>(mask) + 1;
    std::vector<uint16_t> adjusted(symbols.size());
    for (int delta = -search_radius; delta <= search_radius; ++delta) {
        int delta_mod = delta % modulus;
        if (delta_mod < 0) {
            delta_mod += modulus;
        }
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            adjusted[i] = static_cast<uint16_t>((symbols[i] + delta_mod) & mask);
        }
        for (std::size_t candidate = 0; candidate + 8 <= adjusted.size(); ++candidate) {
            auto header = try_decode_header(adjusted, candidate, meta);
            if (header.success) {
                return static_cast<uint16_t>(delta_mod);
            }
        }
    }
    return std::nullopt;
}

std::optional<BruteForceCfoResult> brute_force_integer_cfo(const std::vector<uint16_t>& symbols,
                                                           const host_sim::LoRaMetadata& meta,
                                                           int search_radius,
                                                           int max_start,
                                                           int min_score,
                                                           std::optional<std::size_t> forced_start)
{
    if (symbols.size() < 8 || meta.sf <= 0 || search_radius <= 0) {
        return std::nullopt;
    }
    const uint16_t mask = static_cast<uint16_t>((1u << meta.sf) - 1u);
    const int modulus = static_cast<int>(mask) + 1;
    std::vector<uint16_t> adjusted(symbols.size());
    int best_score = -1;
    uint16_t best_offset = 0;
    std::size_t best_start = 0;

    for (int delta = -search_radius; delta <= search_radius; ++delta) {
        int delta_mod = delta % modulus;
        if (delta_mod < 0) {
            delta_mod += modulus;
        }
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            adjusted[i] = static_cast<uint16_t>((symbols[i] + delta_mod) & mask);
        }
        const std::size_t max_candidate =
            std::min<std::size_t>(adjusted.size(), static_cast<std::size_t>(max_start + 8));
        auto consider_start = [&](std::size_t start) {
            auto header = try_decode_header(adjusted, start, meta);
            if (!header.success) {
                return;
            }
            const int score = header.payload_len;
            if (score > best_score) {
                best_score = score;
                best_offset = static_cast<uint16_t>(delta_mod);
                best_start = start;
            }
        };
        if (forced_start) {
            consider_start(*forced_start);
        } else {
            for (std::size_t start = 0; start + 8 <= max_candidate; ++start) {
                consider_start(start);
            }
        }
    }
    if (best_score >= min_score) {
        return BruteForceCfoResult{best_offset, best_start, best_score};
    }
    return std::nullopt;
}

std::optional<uint16_t> brute_force_integer_cfo(const std::vector<uint16_t>& symbols,
                                                const host_sim::LoRaMetadata& meta,
                                                int max_offset,
                                                int max_start)
{
    if (symbols.size() < 8 || meta.sf <= 0 || max_offset <= 0 || max_start < 0) {
        return std::nullopt;
    }
    const uint16_t mask = static_cast<uint16_t>((1u << meta.sf) - 1u);
    const int modulus = static_cast<int>(mask) + 1;
    std::vector<uint16_t> adjusted(symbols.size());
    for (int delta = -max_offset; delta <= max_offset; ++delta) {
        int delta_mod = delta % modulus;
        if (delta_mod < 0) {
            delta_mod += modulus;
        }
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            adjusted[i] = static_cast<uint16_t>((symbols[i] + delta_mod) & mask);
        }
        const std::size_t max_candidate =
            std::min<std::size_t>(adjusted.size(), static_cast<std::size_t>(max_start + 8));
        for (std::size_t start = 0; start + 8 <= max_candidate; ++start) {
            auto header = try_decode_header(adjusted, start, meta);
            if (header.success) {
                return static_cast<uint16_t>(delta_mod);
            }
        }
    }
    return std::nullopt;
}

} // namespace host_sim::lora_replay
