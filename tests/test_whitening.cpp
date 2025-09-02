#include <gtest/gtest.h>
#include "lora/utils/whitening.hpp"
#include <vector>
#include <cstdint>
using namespace lora::utils;

TEST(Whitening, Roundtrip) {
    std::vector<uint8_t> buf(1024);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(i * 31u + 7u);

    auto w1 = LfsrWhitening::pn9_default();
    auto w2 = LfsrWhitening::pn9_default();

    std::vector<uint8_t> enc = buf;
    w1.apply(enc.data(), enc.size());
    w2.apply(enc.data(), enc.size()); // dewhiten = whiten שוב

    EXPECT_EQ(enc, buf);
}
