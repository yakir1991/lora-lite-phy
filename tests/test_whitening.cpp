#include <gtest/gtest.h>
#include "lora/utils/whitening.hpp"
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
