#include <gtest/gtest.h>
#include <vector>
#include <complex>
#include <algorithm>
#include "lora/constants.hpp"
#include "lora/tx/loopback_tx.hpp"
#include "lora/rx/loopback_rx.hpp"
#include "lora/utils/gray.hpp"

using namespace lora;
using namespace lora::utils;

TEST(RxSyncWord, Mismatch) {
    Workspace ws;
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    uint32_t sf = 7;
    CodeRate cr = CodeRate::CR45;

    // Generate payload IQ samples
    auto txsig = lora::tx::loopback_tx(ws, payload, sf, cr);

    // Prefix correct sync word
    std::vector<std::complex<float>> sig(ws.N + txsig.size());
    uint32_t sync_sym = lora::utils::gray_encode(LORA_SYNC_WORD_PUBLIC);
    for (uint32_t n = 0; n < ws.N; ++n)
        sig[n] = ws.upchirp[(n + sync_sym) % ws.N];
    std::copy(txsig.begin(), txsig.end(), sig.begin() + ws.N);

    // Verify successful decode when sync word matches
    auto ok = lora::rx::loopback_rx(ws, sig, sf, cr, payload.size(), true);
    ASSERT_TRUE(ok.second);

    // Corrupt sync word and expect failure
    uint32_t bad_sym = lora::utils::gray_encode(0x00);
    for (uint32_t n = 0; n < ws.N; ++n)
        sig[n] = ws.upchirp[(n + bad_sym) % ws.N];
    auto bad = lora::rx::loopback_rx(ws, sig, sf, cr, payload.size(), true);
    EXPECT_FALSE(bad.second);
}

