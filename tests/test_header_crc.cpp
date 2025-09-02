#include <gtest/gtest.h>
#include "lora/utils/header_crc.hpp"
#include <vector>
#include <cstdint>
using namespace lora::utils;

TEST(HeaderCRC, RoundtripBE) {
    std::vector<uint8_t> hdr = {
        0x1A,        // payload length (example)
        0b00111001,  // CR=4/7 (3), CRC presence=1 (synthetic layout)
        0x55         // arbitrary
    };
    HeaderCrc::append_trailer_be(hdr);
    EXPECT_TRUE(HeaderCrc::verify_be(hdr.data(), hdr.size()));
}