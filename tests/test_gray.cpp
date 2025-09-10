#include <gtest/gtest.h>
#include "lora/utils/gray.hpp"
using namespace lora::utils;

TEST(Gray, Roundtrip) {
    for (uint32_t x = 0; x < (1u<<16); x += 73) {
        uint32_t g = gray_encode(x);
        uint32_t d = gray_decode(g);
        EXPECT_EQ(d, x);
    }
}
TEST(Gray, TableInverse) {
    auto inv = make_gray_inverse_table(256);
    for (uint32_t x = 0; x < 256; ++x) {
        EXPECT_EQ(inv[gray_encode(x)], x);
    }
}