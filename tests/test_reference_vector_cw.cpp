#include "lora/rx/header_decode.hpp"
#include "lora/utils/gray.hpp"
#include "lora/utils/hamming.hpp"
#include "lora/workspace.hpp"
#include <complex>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <vector>

using namespace lora;
using namespace lora::rx;
using namespace lora::utils;

TEST(ReferenceVectorCW, MatchesKnownSequence) {
  namespace fs = std::filesystem;
  Workspace ws;
  auto root = fs::path(__FILE__).parent_path().parent_path();
  auto vec = root / "vectors" /
             "bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown";
  std::ifstream iqf(vec, std::ios::binary);
  ASSERT_TRUE(iqf.good());
  std::vector<std::complex<float>> iq;
  float buf[2];
  while (iqf.read(reinterpret_cast<char *>(buf), sizeof(buf))) {
    iq.emplace_back(buf[0], buf[1]);
  }
  auto hdr_opt = decode_header_with_preamble_cfo_sto_os_impl(
      ws, std::span<const std::complex<float>>(iq.data(), iq.size()), 7,
      CodeRate::CR45, 8, 0x12);
  ASSERT_TRUE(hdr_opt.has_value());
  const uint32_t N = 1u << 7;
  uint32_t g0[8]{}, g1[8]{};
  for (int s = 0; s < 8; ++s)
    g0[s] = ((ws.dbg_hdr_syms_raw[s] + N - 1u) & (N - 1u)) >> 2;
  for (int s = 0; s < 8; ++s)
    g1[s] = ((ws.dbg_hdr_syms_raw[8 + s] + N - 1u) & (N - 1u)) >> 2;
  auto build_block_rows = [&](const uint32_t gnu[8], uint8_t (&rows)[5][8]) {
    const uint32_t sf_app = 5;
    const uint32_t cw_len = 8u;
    std::vector<std::vector<uint8_t>> inter(cw_len,
                                            std::vector<uint8_t>(sf_app));
    for (uint32_t i = 0; i < cw_len; ++i) {
      uint32_t g = lora::utils::gray_encode(gnu[i]);
      uint32_t sub = g & ((1u << sf_app) - 1u);
      for (uint32_t j = 0; j < sf_app; ++j)
        inter[i][j] = (sub >> (sf_app - 1u - j)) & 1u;
    }
    std::vector<std::vector<uint8_t>> de(sf_app, std::vector<uint8_t>(cw_len));
    for (uint32_t i = 0; i < cw_len; ++i) {
      for (uint32_t j = 0; j < sf_app; ++j) {
        int r = static_cast<int>(i) - static_cast<int>(j) - 1;
        r %= static_cast<int>(sf_app);
        if (r < 0)
          r += static_cast<int>(sf_app);
        de[static_cast<size_t>(r)][i] = inter[i][j];
      }
    }
    for (uint32_t r = 0; r < sf_app; ++r)
      for (uint32_t c = 0; c < cw_len; ++c)
        rows[r][c] = de[r][c];
  };
  uint8_t b0[5][8]{}, b1[5][8]{};
  build_block_rows(g0, b0);
  build_block_rows(g1, b1);
  uint8_t cw[10]{};
  for (uint32_t r = 0; r < 5; ++r) {
    uint16_t c = 0;
    for (uint32_t i = 0; i < 8; ++i)
      c = (c << 1) | (b0[r][i] & 1u);
    cw[r] = static_cast<uint8_t>(c & 0xFF);
  }
  for (uint32_t r = 0; r < 5; ++r) {
    uint16_t c = 0;
    for (uint32_t i = 0; i < 8; ++i)
      c = (c << 1) | (b1[r][i] & 1u);
    cw[5 + r] = static_cast<uint8_t>(c & 0xFF);
  }
  uint8_t expect[10] = {0x00, 0x74, 0xC5, 0x00, 0xC5,
                        0x1D, 0x12, 0x1B, 0x12, 0x00};
  for (int i = 0; i < 10; ++i)
    EXPECT_EQ(cw[i], expect[i]);
}
