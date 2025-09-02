#include <gtest/gtest.h>
#include "lora/utils/interleaver.hpp"
using namespace lora::utils;

TEST(Interleaver, DimensionsPlaceholder) {
    auto M = make_diagonal_interleaver(7, 4+3); // SF7, CR=4/7
    EXPECT_EQ(M.n_in, 7u);
    EXPECT_EQ(M.n_out, 10u);
    EXPECT_EQ(M.map.size(), 10u);
}