#include <gtest/gtest.h>
#include "lora/utils/whitening.hpp"
#include "lora/utils/crc.hpp"
#include <vector>
#include <cstdint>
using namespace lora::utils;

// Whitening must be reversible for any input.  Uniform patterns of all zeroes or
// ones are particularly important regression cases because earlier
// implementations mishandled the PN sequence reset, producing data-dependent
// corruption.  These tests make sure that whitening remains an involution even
// for those edge cases.

TEST(Whitening, Roundtrip) {
    std::vector<uint8_t> buf(1024);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(i * 31u + 7u);

    auto w1 = LfsrWhitening::pn9_default();
    auto w2 = LfsrWhitening::pn9_default();

    std::vector<uint8_t> enc = buf;
    w1.apply(enc.data(), enc.size());
    w2.apply(enc.data(), enc.size()); // dewhiten = re-apply whitening

    EXPECT_EQ(enc, buf);
}

namespace {
void roundtrip_pattern(size_t len, uint8_t value) {
    std::vector<uint8_t> buf(len, value);
    auto w1 = LfsrWhitening::pn9_default();
    auto w2 = LfsrWhitening::pn9_default();
    std::vector<uint8_t> enc = buf;
    w1.apply(enc.data(), enc.size());
    w2.apply(enc.data(), enc.size());
    EXPECT_EQ(enc, buf);
}
} // namespace

TEST(Whitening, UniformPatternsRegression) {
    roundtrip_pattern(1, 0x00);
    roundtrip_pattern(20, 0x00);
    roundtrip_pattern(255, 0x00);
    roundtrip_pattern(1, 0xFF);
    roundtrip_pattern(20, 0xFF);
    roundtrip_pattern(255, 0xFF);
}

TEST(Whitening, PayloadOnlyExcludesCRC) {
    std::vector<uint8_t> payload{0x01, 0x02, 0x03};
    lora::utils::Crc16Ccitt crc;
    uint16_t c = crc.compute(payload.data(), payload.size());
    payload.push_back(static_cast<uint8_t>(c & 0xFF));
    payload.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
    auto crc0 = payload[payload.size()-2];
    auto crc1 = payload[payload.size()-1];
    auto w = LfsrWhitening::pn9_default();
    w.apply(payload.data(), payload.size()-2);
    EXPECT_EQ(payload[payload.size()-2], crc0);
    EXPECT_EQ(payload[payload.size()-1], crc1);
}

TEST(Whitening, MSBFirstMask) {
    uint8_t buf[2] = {0x00, 0x00};
    auto w = LfsrWhitening::pn9_default();
    w.apply(buf, 2);
    EXPECT_EQ(buf[0], 0xFF);
    EXPECT_EQ(buf[1], 0x83);
}
