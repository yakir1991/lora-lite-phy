/// Header decoding and CRC helpers for the LoRa replay pipeline.
///
/// These live in the library rather than the lora_replay CLI so that other
/// consumers — the security auditor, and firmware embedding the decoder — can
/// use the declarations in host_sim/lora_replay/header_decoder.hpp.

#include "host_sim/deinterleaver.hpp"
#include "host_sim/gray.hpp"
#include "host_sim/hamming.hpp"
#include "host_sim/lora_replay/header_decoder.hpp"
#include "host_sim/lora_replay/stage_processing.hpp"
#include "host_sim/whitening.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <vector>

namespace host_sim::lora_replay
{

// Local CRC-16/CCITT: computes CRC over ALL input bytes (no XOR with trailing bytes).
// Different from host_sim::lora_replay::compute_lora_crc which includes the
// gr-lora_sdr last-2-byte XOR.  Used by probe_payload_crc which does the XOR manually.
uint16_t compute_raw_crc16(const std::vector<uint8_t>& payload)
{
    uint16_t crc = 0x0000;
    for (uint8_t byte : payload)
    {
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit)
        {
            if (crc & 0x8000)
            {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return crc;
}

HeaderDecodeResult try_decode_header(const std::vector<uint16_t>& symbols, std::size_t start,
                                     const host_sim::LoRaMetadata& meta)
{
    static const bool debug_header = (std::getenv("HOST_SIM_DEBUG_HEADER") != nullptr);
    HeaderDecodeResult result;
    if (start + 8 > symbols.size())
    {
        return result;
    }

    host_sim::DeinterleaverConfig header_cfg{meta.sf, 4, true, meta.ldro};
    const int block_symbols = 8;
    std::size_t cursor = start;
    std::size_t total_consumed = 0;
    std::vector<uint8_t> header_nibbles;
    std::vector<uint16_t> header_codewords;
    while (header_nibbles.size() < 5 && cursor + block_symbols <= symbols.size())
    {
        std::vector<uint16_t> header_input(symbols.begin() + cursor,
                                           symbols.begin() + cursor + block_symbols);
        if (debug_header)
        {
            std::cout << "Header symbols (start=" << cursor << "):";
            for (auto value : header_input)
            {
                std::cout << ' ' << value;
            }
            std::cout << "\n";
        }

        std::size_t consumed_block = 0;
        auto codewords = host_sim::deinterleave(header_input, header_cfg, consumed_block);
        if (consumed_block == 0)
        {
            break;
        }
        total_consumed += consumed_block;
        cursor += consumed_block;
        header_codewords.insert(header_codewords.end(), codewords.begin(), codewords.end());

        if (debug_header)
        {
            std::cout << "Deinterleaved codewords:";
            for (auto cw : codewords)
            {
                std::cout << ' ' << std::hex << static_cast<int>(cw) << std::dec;
            }
            std::cout << "\n";
        }

        auto nibbles = host_sim::hamming_decode_block(codewords, true, 4);
        if (debug_header)
        {
            std::cout << "Header nibbles:";
            for (auto nib : nibbles)
            {
                std::cout << ' ' << std::hex << static_cast<int>(nib & 0xF) << std::dec;
            }
            std::cout << "\n";
        }
        header_nibbles.insert(header_nibbles.end(), nibbles.begin(), nibbles.end());
    }

    if (header_nibbles.size() < 5)
    {
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

    const bool c4 = ((n0 & 0x8) >> 3) ^ ((n0 & 0x4) >> 2) ^ ((n0 & 0x2) >> 1) ^ (n0 & 0x1);
    const bool c3 =
        ((n0 & 0x8) >> 3) ^ ((n1 & 0x8) >> 3) ^ ((n1 & 0x4) >> 2) ^ ((n1 & 0x2) >> 1) ^ (n2 & 0x1);
    const bool c2 =
        ((n0 & 0x4) >> 2) ^ ((n1 & 0x8) >> 3) ^ (n1 & 0x1) ^ ((n2 & 0x8) >> 3) ^ ((n2 & 0x2) >> 1);
    const bool c1 = ((n0 & 0x2) >> 1) ^ ((n1 & 0x4) >> 2) ^ (n1 & 0x1) ^ ((n2 & 0x4) >> 2) ^
                    ((n2 & 0x2) >> 1) ^ (n2 & 0x1);
    const bool c0 = (n0 & 0x1) ^ ((n1 & 0x2) >> 1) ^ ((n2 & 0x8) >> 3) ^ ((n2 & 0x4) >> 2) ^
                    ((n2 & 0x2) >> 1) ^ (n2 & 0x1);
    const int computed_checksum = (static_cast<int>(c4) << 4) | (static_cast<int>(c3) << 3) |
                                  (static_cast<int>(c2) << 2) | (static_cast<int>(c1) << 1) |
                                  static_cast<int>(c0);

    result.checksum_field = header_chk;
    result.checksum_computed = computed_checksum;

    if (payload_len <= 0 || header_chk != computed_checksum)
    {
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

// Quick CRC probe: decode payload from symbols and check CRC.
// Used for data-start timing refinement at low oversampling,
// where SFO-induced timing drift can shift data symbols by ±1 bin.
bool probe_payload_crc(const std::vector<uint16_t>& symbols, const HeaderDecodeResult& hdr,
                       const host_sim::LoRaMetadata& meta)
{
    if (!hdr.success)
        return false;
    const int pl = hdr.payload_len > 0 ? hdr.payload_len : meta.payload_len;
    const int cr = hdr.cr > 0 ? hdr.cr : meta.cr;
    const bool has_crc = hdr.has_crc || meta.has_crc;
    if (!has_crc || pl < 3)
        return false;

    std::vector<uint8_t> payload_nibbles;
    if (hdr.nibbles.size() > 5)
    {
        for (std::size_t i = 5; i < hdr.nibbles.size(); ++i)
        {
            payload_nibbles.push_back(hdr.nibbles[i]);
        }
    }

    const std::size_t nibble_target = static_cast<std::size_t>(pl) * 2 + 4;
    const int cw_len = cr + 4;
    std::size_t cursor = hdr.consumed_symbols > 0 ? hdr.consumed_symbols : 8;
    host_sim::DeinterleaverConfig payload_cfg{meta.sf, cr, false, meta.ldro};

    while (cursor + static_cast<std::size_t>(cw_len) <= symbols.size() &&
           payload_nibbles.size() < nibble_target)
    {
        std::vector<uint16_t> block(symbols.begin() + static_cast<std::ptrdiff_t>(cursor),
                                    symbols.begin() + static_cast<std::ptrdiff_t>(cursor + cw_len));
        std::size_t consumed = 0;
        auto codewords = host_sim::deinterleave(block, payload_cfg, consumed);
        if (consumed == 0)
            break;
        cursor += consumed;
        auto nibs = host_sim::hamming_decode_block(codewords, false, cr);
        payload_nibbles.insert(payload_nibbles.end(), nibs.begin(), nibs.end());
    }

    if (payload_nibbles.size() < static_cast<std::size_t>(pl) * 2 + 4)
        return false;

    host_sim::WhiteningSequencer seq;
    auto whitening = seq.sequence(payload_nibbles.size() / 2);

    std::vector<uint8_t> unwhitened;
    for (std::size_t i = 0;
         i + 1 < payload_nibbles.size() && unwhitened.size() < static_cast<std::size_t>(pl) + 2;
         i += 2)
    {
        const std::size_t byte_idx = i / 2;
        uint8_t lo, hi;
        if (byte_idx < static_cast<std::size_t>(pl))
        {
            const uint8_t w = (byte_idx < whitening.size()) ? whitening[byte_idx] : 0;
            lo = (payload_nibbles[i] & 0xF) ^ (w & 0x0F);
            hi = (payload_nibbles[i + 1] & 0xF) ^ ((w >> 4) & 0x0F);
        }
        else
        {
            lo = payload_nibbles[i] & 0xF;
            hi = payload_nibbles[i + 1] & 0xF;
        }
        unwhitened.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }

    if (unwhitened.size() < static_cast<std::size_t>(pl) + 2)
        return false;

    std::vector<uint8_t> crc_data(unwhitened.begin(), unwhitened.begin() + pl - 2);
    uint16_t computed = compute_raw_crc16(crc_data);
    if (pl >= 2)
    {
        computed ^= unwhitened[pl - 1];
        computed ^= static_cast<uint16_t>(unwhitened[pl - 2]) << 8;
    }
    const uint16_t decoded =
        static_cast<uint16_t>(unwhitened[pl]) | (static_cast<uint16_t>(unwhitened[pl + 1]) << 8);
    return computed == decoded;
}

} // namespace host_sim::lora_replay
