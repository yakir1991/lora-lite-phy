#pragma once
#include <cstdint>
#include <cstddef>
#include <utility>
#include <vector>
#include "lora/utils/crc.hpp"

namespace lora::utils {

struct HeaderCrc {
    static inline uint16_t compute(const uint8_t* hdr, size_t len,
                                   const Crc16Ccitt& cfg = Crc16Ccitt{}) {
        return cfg.compute(hdr, len);
    }
    static inline std::pair<uint8_t,uint8_t> make_trailer_be(const uint8_t* hdr, size_t len,
                                                             const Crc16Ccitt& cfg = Crc16Ccitt{}) {
        return cfg.make_trailer_be(hdr, len);
    }
    static inline bool verify_be(const uint8_t* hdr_with_crc, size_t len_with_crc,
                                 const Crc16Ccitt& cfg = Crc16Ccitt{}) {
        auto [ok, _] = cfg.verify_with_trailer_be(hdr_with_crc, len_with_crc);
        return ok;
    }
    static inline void append_trailer_be(std::vector<uint8_t>& hdr,
                                         const Crc16Ccitt& cfg = Crc16Ccitt{}) {
        auto [msb, lsb] = cfg.make_trailer_be(hdr.data(), hdr.size());
        hdr.push_back(msb);
        hdr.push_back(lsb);
    }
};

} // namespace lora::utils
