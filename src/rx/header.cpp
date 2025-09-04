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

} // namespace lora::rx

