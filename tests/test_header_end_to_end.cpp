#include <gtest/gtest.h>
#include <vector>
#include <complex>
#include <algorithm>
#include "lora/workspace.hpp"
#include "lora/tx/frame_tx.hpp"
#include "lora/rx/frame.hpp"
#include "lora/utils/gray.hpp"
#include "lora/constants.hpp"

using namespace lora;
using namespace lora::utils;

TEST(HeaderEndToEnd, PreambleSyncDecode) {
  Workspace ws;
  uint32_t sf = 7;
  CodeRate cr = CodeRate::CR47;
  std::vector<uint8_t> payload(32);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i);
  lora::rx::LocalHeader hdr{ .payload_len = static_cast<uint8_t>(payload.size()), .cr = cr, .has_crc = true };

  // Generate frame IQ (no preamble)
  auto iq = lora::tx::frame_tx(ws, payload, sf, cr, hdr);

  // Build preamble (8 upchirps) + sync word symbol + frame IQ
  ws.init(sf);
  uint32_t N = ws.N;
  const size_t pre_len = 8;
  std::vector<std::complex<float>> sig((pre_len + 1) * N + iq.size());
  for (size_t s = 0; s < pre_len; ++s)
    for (uint32_t n = 0; n < N; ++n) sig[s * N + n] = ws.upchirp[n];
  uint32_t sync_sym = lora::utils::gray_encode(lora::LORA_SYNC_WORD_PUBLIC);
  for (uint32_t n = 0; n < N; ++n)
    sig[pre_len * N + n] = ws.upchirp[(n + sync_sym) % N];
  std::copy(iq.begin(), iq.end(), sig.begin() + (pre_len + 1) * N);

  auto out = lora::rx::decode_frame_with_preamble(ws, sig, sf, cr, payload.size(), pre_len);
  ASSERT_TRUE(out.second);
  ASSERT_EQ(out.first.size(), payload.size());
  EXPECT_TRUE(std::equal(out.first.begin(), out.first.end(), payload.begin()));
}

