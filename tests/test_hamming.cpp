#include <gtest/gtest.h>
#include "lora/utils/hamming.hpp"
using namespace lora::utils;

TEST(Hamming, EncodeDecodePlaceholder) {
    auto T = make_placeholder_tables();
    for (int d = 0; d < 16; ++d) {
        auto [cw5, n5] = hamming_encode4(d, CodeRate::CR45, T);
        auto dec5 = hamming_decode4(cw5, n5, CodeRate::CR45, T);
        ASSERT_TRUE(dec5.has_value());
        EXPECT_EQ(dec5->first, (d & 0xF));
    }
}
