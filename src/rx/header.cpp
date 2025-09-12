#include "lora/rx/header.hpp"
#include "lora/utils/crc.hpp"

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

// Parse standard LoRa header format represented as 5 nibble-coded bytes:
// hdr[0] = len_hi (low 4 bits used)
// hdr[1] = len_lo (low 4 bits used)
// hdr[2] = flags  (low 4 bits used): bit0=has_crc, bits[3:1]=cr_idx (1..4)
// hdr[3] = checksum MSB (only bit0 used -> c4)
// hdr[4] = checksum LSB nibble (c3..c0 in low 4 bits)
std::optional<LocalHeader> parse_standard_lora_header(const uint8_t* hdr, size_t len) {
    if (len < 5) return std::nullopt; // need 5 entries

    // Extract nibble-coded fields
    const uint8_t n0 = (hdr[0] & 0x0F); // len_hi
    const uint8_t n1 = (hdr[1] & 0x0F); // len_lo
    const uint8_t n2 = (hdr[2] & 0x0F); // flags nibble
    const uint8_t chk_rx = static_cast<uint8_t>(((hdr[3] & 0x01) << 4) | (hdr[4] & 0x0F));

    // Reconstruct payload length (8 bits)
    const uint8_t payload_len = static_cast<uint8_t>((n0 << 4) | n1);

    // Flags
    const bool has_crc = (n2 & 0x1u) != 0u;
    const uint8_t cr_idx = static_cast<uint8_t>((n2 >> 1) & 0x7u);

    // Compute checksum bits as used in TX generation
    const uint8_t chk_calc = lora::utils::crc_header(n0, n1, n2);

    if (chk_rx != chk_calc) return std::nullopt;
    if (payload_len == 0) return std::nullopt;

    LocalHeader h;
    h.payload_len = payload_len;
    h.has_crc = has_crc;
    h.cr = index_to_cr(cr_idx);
    return h;
}

} // namespace lora::rx

