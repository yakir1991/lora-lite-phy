#include "lora/rx/header.hpp"

namespace lora::rx {

static inline uint8_t cr_to_index(lora::utils::CodeRate cr) {
    switch (cr) {
        case lora::utils::CodeRate::CR45: return 1;
        case lora::utils::CodeRate::CR46: return 2;
        case lora::utils::CodeRate::CR47: return 3;
        case lora::utils::CodeRate::CR48: return 4;
    }
    return 1;
}

static inline lora::utils::CodeRate index_to_cr(uint8_t idx) {
    switch (idx) {
        case 1: return lora::utils::CodeRate::CR45;
        case 2: return lora::utils::CodeRate::CR46;
        case 3: return lora::utils::CodeRate::CR47;
        case 4: return lora::utils::CodeRate::CR48;
        default: return lora::utils::CodeRate::CR45;
    }
}

std::vector<uint8_t> make_local_header_bytes(const LocalHeader& h) {
    uint8_t flags = 0;
    flags |= (h.has_crc ? 1u : 0u);
    flags |= (uint8_t(cr_to_index(h.cr)) & 0x7u) << 1;
    return { h.payload_len, flags };
}

std::vector<uint8_t> make_local_header_with_crc(const LocalHeader& h) {
    auto hdr = make_local_header_bytes(h);
    lora::utils::HeaderCrc::append_trailer_be(hdr);
    return hdr;
}

std::optional<LocalHeader> parse_local_header_with_crc(const uint8_t* hdr_with_crc,
                                                       size_t len_with_crc) {
    if (len_with_crc < 4) return std::nullopt; // 2 bytes + 2 CRC
    if (!lora::utils::HeaderCrc::verify_be(hdr_with_crc, len_with_crc))
        return std::nullopt;
    LocalHeader h;
    h.payload_len = hdr_with_crc[0];
    uint8_t flags = hdr_with_crc[1];
    h.has_crc = (flags & 0x1u) != 0u;
    h.cr = index_to_cr((flags >> 1) & 0x7u);
    return h;
}

// Parse standard LoRa header format (5 bytes: 2 payload_len + 1 flags + 2 checksum)
std::optional<LocalHeader> parse_standard_lora_header(const uint8_t* hdr, size_t len) {
    if (len < 5) return std::nullopt; // 5 bytes total
    
    // Extract payload length (8 bits from first 2 bytes)
    uint8_t payload_len = (hdr[0] << 4) + hdr[1];
    
    // Extract flags
    uint8_t flags = hdr[2];
    bool has_crc = (flags & 0x1u) != 0u;
    uint8_t cr_idx = (flags >> 1) & 0x7u;
    
    // Extract header checksum (5 bits from last 2 bytes)
    uint8_t header_chk = ((hdr[3] & 1) << 4) + hdr[4];
    
    // Verify header checksum using GNU Radio's algorithm
    bool c4 = (hdr[0] & 0b1000) >> 3 ^ (hdr[0] & 0b0100) >> 2 ^ (hdr[0] & 0b0010) >> 1 ^ (hdr[0] & 0b0001);
    bool c3 = (hdr[0] & 0b1000) >> 3 ^ (hdr[1] & 0b1000) >> 3 ^ (hdr[1] & 0b0100) >> 2 ^ (hdr[1] & 0b0010) >> 1 ^ (hdr[2] & 0b0001);
    bool c2 = (hdr[0] & 0b0100) >> 2 ^ (hdr[1] & 0b1000) >> 3 ^ (hdr[1] & 0b0001) ^ (hdr[2] & 0b1000) >> 3 ^ (hdr[2] & 0b0010) >> 1;
    bool c1 = (hdr[0] & 0b0010) >> 1 ^ (hdr[1] & 0b0100) >> 2 ^ (hdr[1] & 0b0001) ^ (hdr[2] & 0b0100) >> 2 ^ (hdr[2] & 0b0010) >> 1 ^ (hdr[2] & 0b0001);
    bool c0 = (hdr[0] & 0b0001) ^ (hdr[1] & 0b0010) >> 1 ^ (hdr[2] & 0b1000) >> 3 ^ (hdr[2] & 0b0100) >> 2 ^ (hdr[2] & 0b0010) >> 1 ^ (hdr[2] & 0b0001);
    
    uint8_t calculated_chk = (c4 << 4) + (c3 << 3) + (c2 << 2) + (c1 << 1) + c0;
    
    // Check if header checksum is valid and payload length is not zero
    if (header_chk != calculated_chk || payload_len == 0) {
        return std::nullopt;
    }
    
    LocalHeader h;
    h.payload_len = payload_len;
    h.has_crc = has_crc;
    h.cr = index_to_cr(cr_idx);
    return h;
}

} // namespace lora::rx

