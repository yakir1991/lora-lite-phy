#include <gtest/gtest.h>
#include "lora/utils/hamming.hpp"
using namespace lora::utils;

TEST(Hamming, EncodeDecodeAllRates) {
    auto T = make_hamming_tables();
    for (int d = 0; d < 16; ++d) {
        for (CodeRate cr : {CodeRate::CR45, CodeRate::CR46, CodeRate::CR47, CodeRate::CR48}) {
            auto [cw, n] = hamming_encode4(d, cr, T);
            auto dec = hamming_decode4(cw, n, cr, T);
            ASSERT_TRUE(dec.has_value());
            EXPECT_EQ(dec->first, (d & 0xF));
            EXPECT_FALSE(dec->second);
        }
    }
}

TEST(Hamming, DetectOrCorrect) {
    auto T = make_hamming_tables();

    // CR 4/5 detection
    auto [cw5, n5] = hamming_encode4(0xA, CodeRate::CR45, T);
    cw5 ^= 1u << 0; // flip a bit
    EXPECT_FALSE(hamming_decode4(cw5, n5, CodeRate::CR45, T).has_value());

    // CR 4/6 detection
    auto [cw6, n6] = hamming_encode4(0xA, CodeRate::CR46, T);
    cw6 ^= 1u << 0;
    EXPECT_FALSE(hamming_decode4(cw6, n6, CodeRate::CR46, T).has_value());

    // CR 4/7 correction
    auto [cw7, n7] = hamming_encode4(0xA, CodeRate::CR47, T);
    for (int i = 0; i < n7; ++i) {
        auto cw_err = cw7 ^ (1u << i);
        auto dec = hamming_decode4(cw_err, n7, CodeRate::CR47, T);
        ASSERT_TRUE(dec.has_value());
        EXPECT_EQ(dec->first, 0xA & 0xF);
        EXPECT_TRUE(dec->second);
    }

    // CR 4/8 correction + double-error detection
    auto [cw8, n8] = hamming_encode4(0xA, CodeRate::CR48, T);
    for (int i = 0; i < n8; ++i) {
        auto cw_err = cw8 ^ (1u << i);
        auto dec = hamming_decode4(cw_err, n8, CodeRate::CR48, T);
        ASSERT_TRUE(dec.has_value());
        EXPECT_EQ(dec->first, 0xA & 0xF);
        EXPECT_TRUE(dec->second);
    }
    auto dbl = cw8 ^ 0x3u; // two-bit error
    EXPECT_FALSE(hamming_decode4(dbl, n8, CodeRate::CR48, T).has_value());
}
