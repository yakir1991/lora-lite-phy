#include <gtest/gtest.h>
#include "lora/utils/crc.hpp"
#include <vector>
#include <cstdint>
using namespace lora::utils;

TEST(CRC16, RoundtripTrailerBE) {
    Crc16Ccitt crc{};
    std::vector<uint8_t> payload(100);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = (i * 17 + 3) & 0xFF;

    auto [msb, lsb] = crc.make_trailer_be(payload.data(), payload.size());
    payload.push_back(msb);
    payload.push_back(lsb);

    auto [ok, calc] = crc.verify_with_trailer_be(payload.data(), payload.size());
    EXPECT_TRUE(ok) << std::hex << calc;
}

TEST(CRC16, KnownVector) {
    Crc16Ccitt crc{};
    const char* msg = "123456789";
    uint16_t calc = crc.compute(reinterpret_cast<const uint8_t*>(msg), 9);
    EXPECT_EQ(calc, 0x29B1);
}
