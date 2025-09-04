#include <gtest/gtest.h>
#include <vector>
#include <complex>
#include "lora/workspace.hpp"
#include "lora/tx/loopback_tx.hpp"
#include "lora/rx/preamble.hpp"
#include "lora/utils/gray.hpp"
#include "lora/constants.hpp"
#include <numeric>
#include <cmath>

using namespace lora;
using namespace lora::utils;

TEST(Preamble, DetectAndDecode) {
  Workspace ws;
  uint32_t sf = 7;
  CodeRate cr = CodeRate::CR47;
  std::vector<uint8_t> payload = {0x10, 0x20, 0x30, 0x40, 0x55, 0xAA};

  // Generate payload IQ
  auto txsig = lora::tx::loopback_tx(ws, payload, sf, cr);

  // Build preamble (8 upchirps) + sync word + data
  ws.init(sf);
  uint32_t N = ws.N;
  const size_t pre_len = 8;
  std::vector<std::complex<float>> sig;
  sig.resize((pre_len + 1) * N + txsig.size());
  // preamble: upchirp (sym == 0)
  for (size_t s = 0; s < pre_len; ++s) {
    for (uint32_t n = 0; n < N; ++n)
      sig[s * N + n] = ws.upchirp[n];
  }
  // sync word symbol
  uint32_t sync_sym = lora::utils::gray_encode(lora::LORA_SYNC_WORD_PUBLIC);
  for (uint32_t n = 0; n < N; ++n)
    sig[pre_len * N + n] = ws.upchirp[(n + sync_sym) % N];
  // append data
  std::copy(txsig.begin(), txsig.end(), sig.begin() + (pre_len + 1) * N);

  // Detect + decode
  auto out = lora::rx::decode_with_preamble(ws, sig, sf, cr, payload.size(), pre_len);
  ASSERT_TRUE(out.second);
  ASSERT_EQ(out.first.size(), payload.size());
  EXPECT_TRUE(std::equal(out.first.begin(), out.first.end(), payload.begin()));
}

TEST(Preamble, DetectFailOnShortPreamble) {
  Workspace ws;
  uint32_t sf = 8;
  CodeRate cr = CodeRate::CR45;
  std::vector<uint8_t> payload = {1,2,3,4};
  auto txsig = lora::tx::loopback_tx(ws, payload, sf, cr);
  ws.init(sf);
  uint32_t N = ws.N;
  const size_t pre_len = 3; // too short
  std::vector<std::complex<float>> sig((pre_len + 1) * N + txsig.size());
  for (size_t s = 0; s < pre_len; ++s)
    for (uint32_t n = 0; n < N; ++n) sig[s * N + n] = ws.upchirp[n];
  uint32_t sync_sym = lora::utils::gray_encode(lora::LORA_SYNC_WORD_PUBLIC);
  for (uint32_t n = 0; n < N; ++n)
    sig[pre_len * N + n] = ws.upchirp[(n + sync_sym) % N];
  std::copy(txsig.begin(), txsig.end(), sig.begin() + (pre_len + 1) * N);
  auto out = lora::rx::decode_with_preamble(ws, sig, sf, cr, payload.size(), 6);
  EXPECT_FALSE(out.second);
}

TEST(Preamble, CFOCompensation) {
  Workspace ws;
  uint32_t sf = 9;
  CodeRate cr = CodeRate::CR48;
  std::vector<uint8_t> payload(32, 0xAB);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= (i & 0xFF);
  auto txsig = lora::tx::loopback_tx(ws, payload, sf, cr);
  ws.init(sf);
  uint32_t N = ws.N;
  const size_t pre_len = 8;
  std::vector<std::complex<float>> sig((pre_len + 1) * N + txsig.size());
  // preamble
  for (size_t s = 0; s < pre_len; ++s)
    for (uint32_t n = 0; n < N; ++n) sig[s * N + n] = ws.upchirp[n];
  // sync
  uint32_t sync_sym = lora::utils::gray_encode(lora::LORA_SYNC_WORD_PUBLIC);
  for (uint32_t n = 0; n < N; ++n)
    sig[pre_len * N + n] = ws.upchirp[(n + sync_sym) % N];
  // data
  std::copy(txsig.begin(), txsig.end(), sig.begin() + (pre_len + 1) * N);
  // Inject CFO: rotate entire sequence by exp(j*2π*ε*n)
  float eps = 5e-4f; // cycles per sample
  std::complex<float> j(0.f, 1.f);
  for (size_t n = 0; n < sig.size(); ++n) {
    float ang = 2.0f * static_cast<float>(M_PI) * eps * static_cast<float>(n);
    sig[n] *= std::exp(j * ang);
  }
  auto out = lora::rx::decode_with_preamble_cfo(ws, sig, sf, cr, payload.size(), pre_len);
  ASSERT_TRUE(out.second);
  ASSERT_EQ(out.first.size(), payload.size());
  EXPECT_TRUE(std::equal(out.first.begin(), out.first.end(), payload.begin()));
}

TEST(Preamble, STOAlignment) {
  Workspace ws;
  uint32_t sf = 8;
  CodeRate cr = CodeRate::CR47;
  std::vector<uint8_t> payload(24);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i*7 + 3);
  auto txsig = lora::tx::loopback_tx(ws, payload, sf, cr);
  ws.init(sf);
  uint32_t N = ws.N;
  const size_t pre_len = 8;
  std::vector<std::complex<float>> sig((pre_len + 1) * N + txsig.size());
  // preamble
  for (size_t s = 0; s < pre_len; ++s)
    for (uint32_t n = 0; n < N; ++n) sig[s * N + n] = ws.upchirp[n];
  // sync
  uint32_t sync_sym = lora::utils::gray_encode(lora::LORA_SYNC_WORD_PUBLIC);
  for (uint32_t n = 0; n < N; ++n)
    sig[pre_len * N + n] = ws.upchirp[(n + sync_sym) % N];
  // data
  std::copy(txsig.begin(), txsig.end(), sig.begin() + (pre_len + 1) * N);
  // Inject integer STO: shift by +13 samples
  int sto = 13;
  std::vector<std::complex<float>> sto_sig(sig.size() + sto);
  std::fill(sto_sig.begin(), sto_sig.begin() + sto, std::complex<float>{0.f,0.f});
  std::copy(sig.begin(), sig.end(), sto_sig.begin() + sto);
  auto out = lora::rx::decode_with_preamble_cfo_sto(ws, sto_sig, sf, cr, payload.size(), pre_len);
  ASSERT_TRUE(out.second);
  ASSERT_EQ(out.first.size(), payload.size());
  EXPECT_TRUE(std::equal(out.first.begin(), out.first.end(), payload.begin()));
}
static std::vector<std::complex<float>> upsample_repeat(const std::vector<std::complex<float>>& x, int os) {
  if (os <= 1) return x;
  std::vector<std::complex<float>> y;
  y.reserve(x.size() * os);
  for (auto v : x) for (int i=0;i<os;++i) y.push_back(v);
  return y;
}
TEST(Preamble, OS4DetectAndDecode) {
  Workspace ws;
  uint32_t sf = 7;
  CodeRate cr = CodeRate::CR46;
  std::vector<uint8_t> payload(20);
  std::iota(payload.begin(), payload.end(), 1);
  auto txsig = lora::tx::loopback_tx(ws, payload, sf, cr);
  ws.init(sf);
  uint32_t N = ws.N;
  const size_t pre_len = 8;
  std::vector<std::complex<float>> sig((pre_len + 1) * N + txsig.size());
  for (size_t s = 0; s < pre_len; ++s)
    for (uint32_t n = 0; n < N; ++n) sig[s * N + n] = ws.upchirp[n];
  uint32_t sync_sym = lora::utils::gray_encode(lora::LORA_SYNC_WORD_PUBLIC);
  for (uint32_t n = 0; n < N; ++n)
    sig[pre_len * N + n] = ws.upchirp[(n + sync_sym) % N];
  std::copy(txsig.begin(), txsig.end(), sig.begin() + (pre_len + 1) * N);
  auto sig_os4 = upsample_repeat(sig, 4);
  auto out = lora::rx::decode_with_preamble_cfo_sto_os(ws, sig_os4, sf, cr, payload.size(), pre_len);
  ASSERT_TRUE(out.second);
  ASSERT_EQ(out.first.size(), payload.size());
  EXPECT_TRUE(std::equal(out.first.begin(), out.first.end(), payload.begin()));
}
