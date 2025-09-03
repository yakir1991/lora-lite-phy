#include "lora/rx/loopback_rx.hpp"
#include "lora/tx/loopback_tx.hpp"
#include "lora/workspace.hpp"
#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <iostream>
#include <random>

using namespace lora;
using namespace lora::utils;

static std::string cr_to_string(CodeRate cr) {
  switch (cr) {
  case CodeRate::CR45:
    return "4/5";
  case CodeRate::CR46:
    return "4/6";
  case CodeRate::CR47:
    return "4/7";
  case CodeRate::CR48:
    return "4/8";
  default:
    return "?";
  }
}

TEST(Loopback, TxRx) {
  std::mt19937 rng(1234);
  std::uniform_int_distribution<int> dist(0, 255);
  Workspace ws;

  const std::vector<uint32_t> sfs = {7, 8, 9, 10, 11, 12};
  const std::vector<CodeRate> crs = {CodeRate::CR45, CodeRate::CR46,
                                     CodeRate::CR47, CodeRate::CR48};
  const std::vector<size_t> payload_lengths = {1, 16, 32, 64, 255};

  for (auto sf : sfs) {
    for (auto cr : crs) {
      for (auto len : payload_lengths) {
        std::vector<uint8_t> payload(len);
        for (auto &b : payload)
          b = dist(rng);

        auto start = std::chrono::steady_clock::now();
        auto txsig = tx::loopback_tx(ws, payload, sf, cr);
        auto rxres = rx::loopback_rx(ws, txsig, sf, cr, payload.size());
        auto end = std::chrono::steady_clock::now();
        auto ms =
            std::chrono::duration<double, std::milli>(end - start).count();

        std::cout << "sf=" << sf << " cr=" << cr_to_string(cr) << " len=" << len
                  << " time=" << ms << "ms\n";

        ASSERT_TRUE(rxres.second);
        ASSERT_EQ(rxres.first.size(), payload.size());
        EXPECT_TRUE(std::equal(rxres.first.begin(), rxres.first.end(),
                               payload.begin()));
      }
    }
  }
}
