#include <gtest/gtest.h>
#include <random>
#include <algorithm>
#include "lora/tx/loopback_tx.hpp"
#include "lora/rx/loopback_rx.hpp"
#include "lora/workspace.hpp"

using namespace lora;
using namespace lora::utils;

TEST(Loopback, TxRx) {
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> dist(0, 255);
    Workspace ws;
    std::vector<uint8_t> payload(16);
    for (uint32_t sf = 7; sf <= 12; ++sf) {
        for (CodeRate cr : {CodeRate::CR45, CodeRate::CR46, CodeRate::CR47, CodeRate::CR48}) {
            for (auto& b : payload) b = dist(rng);
            auto txsig = tx::loopback_tx(ws, payload, sf, cr);
            auto rxres = rx::loopback_rx(ws, txsig, sf, cr, payload.size());
            EXPECT_TRUE(rxres.second);
            ASSERT_EQ(rxres.first.size(), payload.size());
            EXPECT_TRUE(std::equal(rxres.first.begin(), rxres.first.end(), payload.begin()));
        }
    }
}

