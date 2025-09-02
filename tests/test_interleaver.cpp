#include <gtest/gtest.h>
#include "lora/utils/interleaver.hpp"
#include <numeric>
#include <vector>

using namespace lora::utils;

namespace {
std::vector<uint8_t> interleave(const std::vector<uint8_t>& in, const InterleaverMap& M) {
    std::vector<uint8_t> out(M.n_out);
    for (size_t i = 0; i < M.n_out; ++i)
        out[i] = in[M.map[i]];
    return out;
}

std::vector<uint8_t> deinterleave(const std::vector<uint8_t>& in, const InterleaverMap& M) {
    std::vector<uint8_t> out(M.n_in);
    for (size_t i = 0; i < M.n_out; ++i)
        out[M.map[i]] = in[i];
    return out;
}
} // namespace

TEST(Interleaver, RoundTripAndLDRO) {
    for (uint32_t sf = 7; sf <= 12; ++sf) {
        for (uint32_t crp4 = 5; crp4 <= 8; ++crp4) {
            auto M = make_diagonal_interleaver(sf, crp4);
            uint32_t n = sf * crp4;
            EXPECT_EQ(M.n_in, n);
            EXPECT_EQ(M.n_out, n);
            EXPECT_EQ(M.map.size(), n);

            std::vector<uint8_t> data(n);
            std::iota(data.begin(), data.end(), 0);
            auto inter = interleave(data, M);
            auto deinter = deinterleave(inter, M);
            EXPECT_EQ(data, deinter);

            uint32_t expected_shift = (sf > 6) ? crp4 : 0;
            EXPECT_EQ(M.map[0], expected_shift) << "sf=" << sf << " cr+4=" << crp4;
        }
    }
}

