#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include "lora/utils/header_crc.hpp"
#include "lora/utils/hamming.hpp"

namespace lora::rx {

struct LocalHeader {
    uint8_t payload_len{};
    lora::utils::CodeRate cr{lora::utils::CodeRate::CR45};
    bool has_crc{true};
};

// Compose a 2-byte local header (LEN, FLAGS) without CRC trailer.
// FLAGS layout (synthetic, for MVP tests):
//   bit0: has_crc
//   bits[3:1]: coding rate index {1..4} mapping to CR45..CR48
std::vector<uint8_t> make_local_header_bytes(const LocalHeader& h);

// Append BE 16-bit CRC trailer over header bytes.
std::vector<uint8_t> make_local_header_with_crc(const LocalHeader& h);

// Verify BE 16-bit CRC trailer and parse fields.
std::optional<LocalHeader> parse_local_header_with_crc(const uint8_t* hdr_with_crc,
                                                       size_t len_with_crc);

} // namespace lora::rx

