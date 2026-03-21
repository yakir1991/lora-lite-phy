#include "host_sim/lora_replay/HeaderSearcher.hpp"
#include "host_sim/lora_replay/header_encoder.hpp"
#include "host_sim/lora_replay/stage_processing.hpp"
#include "host_sim/lora_replay/cfo_estimator.hpp"

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <limits>

namespace host_sim::lora_replay
{

HeaderSearcher::HeaderSearcher(const host_sim::LoRaMetadata& metadata, bool debug_headers)
    : metadata_(metadata), debug_headers_(debug_headers)
{
}

HeaderSearchResult HeaderSearcher::search(const std::vector<uint16_t>& symbols,
                                          std::optional<std::size_t> forced_header_start,
                                          bool external_sync_active)
{
    HeaderSearchResult result;
    HeaderDecodeResult best_header{};
    std::size_t best_header_offset = std::numeric_limits<std::size_t>::max();
    int best_header_score = std::numeric_limits<int>::min();
    bool header_found = false;

    auto attempt = [&](std::size_t candidate) -> bool {
        if (debug_headers_) {
            std::cout << "[header-debug] HeaderSearcher attempt " << candidate << std::endl;
        }
        return attempt_header(symbols, candidate, forced_header_start, result, best_header, best_header_offset, best_header_score);
    };

    auto search_range = [&](std::size_t start, std::size_t end) {
        for (std::size_t candidate = start; candidate + 8 <= end; ++candidate) {
            if (attempt(candidate)) {
                header_found = true;
                break;
            }
        }
    };

    auto local_search_for_exact = [&](std::size_t anchor, std::size_t radius) -> bool {
        const std::size_t window_start = (anchor > radius) ? (anchor - radius) : 0;
        const std::size_t window_end = std::min(symbols.size(), anchor + radius + 1);
        for (std::size_t candidate = window_start; candidate + 8 <= window_end; ++candidate) {
            auto candidate_header = try_decode_header(symbols, candidate, metadata_);
            if (!candidate_header.success) continue;
            if (candidate_header.payload_len == metadata_.payload_len &&
                candidate_header.cr == metadata_.cr &&
                (!metadata_.has_crc || candidate_header.has_crc)) {
                result.header = std::move(candidate_header);
                result.symbol_cursor = candidate + result.header.consumed_symbols;
                result.chosen_offset = candidate;
                result.success = true;
                header_found = true;
                return true;
            }
        }
        return false;
    };

    if (forced_header_start && *forced_header_start + 8 <= symbols.size()) {
        if (external_sync_active) {
            const std::size_t anchor = *forced_header_start;
            const int deltas[] = {0, -1, 1, -2, 2, -3, 3, -4, 4};
            for (int delta : deltas) {
                if (delta < 0 && anchor < static_cast<std::size_t>(-delta)) continue;
                const std::size_t candidate = static_cast<std::size_t>(static_cast<long long>(anchor) + delta);
                if (candidate + 8 > symbols.size()) continue;
                auto probe = try_decode_header(symbols, candidate, metadata_);
                if (probe.success &&
                    probe.payload_len == metadata_.payload_len &&
                    probe.cr == metadata_.cr &&
                    (!metadata_.has_crc || probe.has_crc)) {
                    result.header = std::move(probe);
                    header_found = true;
                    result.chosen_offset = candidate;
                    result.symbol_cursor = result.chosen_offset + result.header.consumed_symbols;
                    result.success = true;
                    std::cout << "[header] exact external-sync match at " << candidate << "\n";
                    break;
                }
            }
            if (!header_found) {
                const std::size_t window_start = (anchor > 8) ? anchor - 8 : 0;
                const std::size_t window_end = std::min<std::size_t>(symbols.size(), anchor + 64);
                HeaderDecodeResult first_checksum_ok{};
                std::size_t first_idx = std::numeric_limits<std::size_t>::max();
                for (std::size_t candidate = window_start; candidate + 8 <= window_end; ++candidate) {
                    auto probe = try_decode_header(symbols, candidate, metadata_);
                    if (!probe.success) continue;
                    if (!first_checksum_ok.success) {
                        first_checksum_ok = probe;
                        first_idx = candidate;
                    }
                    if (probe.payload_len == metadata_.payload_len &&
                        probe.cr == metadata_.cr &&
                        (!metadata_.has_crc || probe.has_crc)) {
                        result.header = std::move(probe);
                        header_found = true;
                        result.checksum_valid = true;
                        result.chosen_offset = candidate;
                        result.symbol_cursor = result.chosen_offset + result.header.consumed_symbols;
                        result.success = true;
                        std::cout << "[header] external sync: checksum-valid match at " << candidate << "\n";
                        break;
                    }
                }
                if (!header_found && first_checksum_ok.success) {
                    result.header = std::move(first_checksum_ok);
                    header_found = true;
                    result.checksum_valid = true;
                    result.chosen_offset = first_idx;
                    result.symbol_cursor = result.chosen_offset + result.header.consumed_symbols;
                    result.success = true;
                    std::cout << "[header] external sync: adopting first checksum-valid header at " << first_idx << "\n";
                }
            }
        } else {
            const std::size_t base_radius = static_cast<std::size_t>(std::max(metadata_.preamble_len + 4, 8));
            const std::size_t radius_small = base_radius;
            const std::size_t radius_large = base_radius + (external_sync_active ? 16 : 8);

            if (!header_found) {
                const std::size_t anchor = *forced_header_start;
                const int deltas[] = {0, -1, 1, -2, 2, -3, 3, -4, 4};
                for (int delta : deltas) {
                    if (delta < 0 && anchor < static_cast<std::size_t>(-delta)) continue;
                    const std::size_t candidate = static_cast<std::size_t>(static_cast<long long>(anchor) + delta);
                    if (candidate + 8 > symbols.size()) continue;
                    auto probe = try_decode_header(symbols, candidate, metadata_);
                    if (probe.success &&
                        probe.payload_len == metadata_.payload_len &&
                        probe.cr == metadata_.cr &&
                        (!metadata_.has_crc || probe.has_crc)) {
                        result.header = std::move(probe);
                        header_found = true;
                        result.chosen_offset = candidate;
                        result.symbol_cursor = result.chosen_offset + result.header.consumed_symbols;
                        result.success = true;
                        std::cout << "[header] exact sync-window match at " << candidate << "\n";
                        break;
                    }
                }
            }

            if (!header_found) local_search_for_exact(*forced_header_start, 1);
            if (!header_found) local_search_for_exact(*forced_header_start, 2);

            auto clamp_window = [&](std::size_t radius) {
                const std::size_t window_start = (*forced_header_start > radius) ? (*forced_header_start - radius) : 0;
                const std::size_t window_end = std::min(symbols.size(), *forced_header_start + radius);
                search_range(window_start, window_end);
            };

            if (!header_found) clamp_window(radius_small);
            if (!header_found) clamp_window(radius_large);
        }
    } else {
        search_range(0, symbols.size());
    }

    if (!result.success && best_header.success && !forced_header_start) {
        result.header = std::move(best_header);
        result.symbol_cursor = best_header_offset + result.header.consumed_symbols;
        result.chosen_offset = best_header_offset;
        result.success = true;
        result.checksum_valid = (result.header.checksum_field == result.header.checksum_computed);
        std::cout << "[header] using best-effort header at candidate " << best_header_offset << "\n";
    }

    if (!result.success && forced_header_start && *forced_header_start + 8 <= symbols.size() && !external_sync_active) {
        result.header.success = true;
        result.header.payload_len = metadata_.payload_len;
        result.header.cr = metadata_.cr;
        result.header.has_crc = metadata_.has_crc;
        result.header.ldro_enabled = (metadata_.sf > 6);
        result.header.consumed_symbols = 8;
        result.header.checksum_field = -1;
        result.header.checksum_computed = -1;
        result.chosen_offset = *forced_header_start;
        result.symbol_cursor = result.chosen_offset + result.header.consumed_symbols;
        result.success = true;
        result.checksum_valid = false;
        std::cout << "[header] checksum decode failed; falling back to metadata at forced start " << result.chosen_offset << "\n";
    }

    return result;
}

bool HeaderSearcher::attempt_header(const std::vector<uint16_t>& symbols,
                                    std::size_t candidate,
                                    std::optional<std::size_t> forced_header_start,
                                    HeaderSearchResult& result,
                                    HeaderDecodeResult& best_header,
                                    std::size_t& best_header_offset,
                                    int& best_header_score)
{
    auto candidate_header = try_decode_header(symbols, candidate, metadata_);
    if (!candidate_header.success) return false;

    auto forced_exact_match = [&]() -> bool {
        if (!forced_header_start || candidate_header.payload_len != metadata_.payload_len
            || candidate_header.cr != metadata_.cr
            || (metadata_.has_crc && !candidate_header.has_crc)) {
            return false;
        }
        const std::size_t forced = *forced_header_start;
        if (candidate == forced ||
            (forced > 0 && candidate + 8 >= forced && candidate <= forced + 2) ||
            (forced >= 1 && candidate + 8 >= forced - 1 && candidate <= forced + 3)) {
            result.header = std::move(candidate_header);
            if (result.header.payload_len <= 0 && metadata_.payload_len > 0) result.header.payload_len = metadata_.payload_len;
            if (result.header.cr <= 0 && metadata_.cr > 0) result.header.cr = metadata_.cr;
            if (!result.header.has_crc && metadata_.has_crc) result.header.has_crc = true;
            result.symbol_cursor = candidate + result.header.consumed_symbols;
            result.chosen_offset = candidate;
            result.success = true;
            return true;
        }
        return false;
    };

    if (forced_exact_match()) return true;

    bool payload_match = (metadata_.payload_len > 0 && candidate_header.payload_len > 0 && candidate_header.payload_len == metadata_.payload_len);
    bool cr_match = (metadata_.cr > 0 && candidate_header.cr > 0 && candidate_header.cr == metadata_.cr);
    bool crc_match = (!metadata_.has_crc || candidate_header.has_crc);

    if (forced_header_start && candidate_header.success && payload_match && cr_match && crc_match) {
        const std::size_t anchor = *forced_header_start;
        if (candidate + 8 >= anchor - 2 && candidate <= anchor + 2) {
            result.header = std::move(candidate_header);
            result.symbol_cursor = candidate + result.header.consumed_symbols;
            result.chosen_offset = candidate;
            result.success = true;
            return true;
        }
    }

    if (payload_match && cr_match && crc_match) {
        result.header = std::move(candidate_header);
        if (result.header.payload_len <= 0 && metadata_.payload_len > 0) result.header.payload_len = metadata_.payload_len;
        if (result.header.cr <= 0 && metadata_.cr > 0) result.header.cr = metadata_.cr;
        if (!result.header.has_crc && metadata_.has_crc) result.header.has_crc = true;
        result.symbol_cursor = candidate + result.header.consumed_symbols;
        result.chosen_offset = candidate;
        result.success = true;
        return true;
    }

    int score = 0;
    if (forced_header_start) {
        const std::size_t dist = (candidate > *forced_header_start) ? (candidate - *forced_header_start) : (*forced_header_start - candidate);
        score -= static_cast<int>(std::min<std::size_t>(dist, 32) * 5);
    }
    if (metadata_.payload_len > 0 && candidate_header.payload_len > 0) {
        const int delta = std::abs(candidate_header.payload_len - metadata_.payload_len);
        score -= 50 + delta * 10;
    } else {
        score -= 100;
    }
    if (payload_match) score += 500;
    if (cr_match) score += 200;
    else if (metadata_.cr > 0 && candidate_header.cr > 0) score -= 100;
    if (crc_match && candidate_header.has_crc) score += 20;
    else if (!crc_match) score -= 50;

    if (!best_header.success || score > best_header_score) {
        best_header = std::move(candidate_header);
        best_header_offset = candidate;
        best_header_score = score;
    }
    return false;
}

} // namespace host_sim::lora_replay
