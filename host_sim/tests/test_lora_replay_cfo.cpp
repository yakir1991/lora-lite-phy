#include "host_sim/lora_replay/cfo_estimator.hpp"

#include <iostream>
#include <vector>

using namespace host_sim::lora_replay;

int main()
{
    host_sim::LoRaMetadata meta{};
    meta.sf = 7;
    meta.sync_word = 0x12;
    meta.preamble_len = 10;

    // detect_sync_pair should find the sequence derived from sync_word.
    const uint16_t mask = static_cast<uint16_t>((1u << meta.sf) - 1u);
    const uint16_t sync_high = static_cast<uint16_t>(((meta.sync_word >> 4) & 0x0F) * 8u) & mask;
    const uint16_t sync_low = static_cast<uint16_t>((meta.sync_word & 0x0F) * 8u) & mask;
    std::vector<uint16_t> symbols = {5, 7, 9, sync_high, sync_low, 1};
    auto sync_result = detect_sync_pair(symbols, meta);
    if (!sync_result || sync_result->sync_high != sync_high || sync_result->sync_low != sync_low) {
        std::cerr << "Failed to detect sync pair\n";
        return 1;
    }

    // detect_integer_cfo should recover the intentional delta.
    std::vector<uint16_t> reference = {10, 20, 30, 40, 50, 60, 70, 80};
    const uint16_t delta = 5;
    std::vector<uint16_t> shifted = reference;
    for (auto& value : shifted) {
        value = static_cast<uint16_t>((value - delta) & mask);
    }
    auto detected_delta = detect_integer_cfo(shifted, reference, meta.sf);
    if (!detected_delta || *detected_delta != delta) {
        std::cerr << "Integer CFO detector failed, got "
                  << (detected_delta ? std::to_string(*detected_delta) : "null") << "\n";
        return 1;
    }
    apply_integer_offset(shifted, *detected_delta, meta.sf);
    if (shifted != reference) {
        std::cerr << "Integer offset application failed\n";
        return 1;
    }

    // infer_integer_cfo_from_preamble should identify the dominant symbol.
    std::vector<uint16_t> preamble(12, 13);
    preamble[3] = 15;
    const uint16_t expected_offset =
        static_cast<uint16_t>(((mask + 1u) - 13u) & mask); // 2^sf - dominant value
    auto preamble_estimate = infer_integer_cfo_from_preamble(preamble, meta);
    if (!preamble_estimate || preamble_estimate->offset != expected_offset
        || preamble_estimate->hits < 8) {
        std::cerr << "Failed to infer preamble CFO offset\n";
        return 1;
    }

    return 0;
}
