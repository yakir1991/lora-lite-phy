#include "lora/rx/loopback_rx.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <complex>

using namespace lora;
using namespace lora::rx;
using namespace lora::utils;

TEST(ReferenceVectorsHeaderOSVarLen, DecodeOS4HeaderAutoVarLengths) {
  namespace fs = std::filesystem;
  Workspace ws;
  auto root = fs::path(__FILE__).parent_path().parent_path();
  auto vec_dir = root / "vectors";
  size_t attempted = 0, decoded = 0;
  for (const auto &entry : fs::directory_iterator(vec_dir)) {
    auto name = entry.path().filename().string();
    if (name.find("_payload_len") == std::string::npos) continue;
    int sf = 0, cr_int = 0, ln = 0;
    if (sscanf(name.c_str(), "sf%d_cr%d_payload_len%d.bin", &sf, &cr_int, &ln) != 3) continue;
    CodeRate cr_enum;
    switch (cr_int) {
      case 45: cr_enum = CodeRate::CR45; break;
      case 46: cr_enum = CodeRate::CR46; break;
      case 47: cr_enum = CodeRate::CR47; break;
      case 48: cr_enum = CodeRate::CR48; break;
      default: continue;
    }
    // Read payload
    std::ifstream pf(entry.path(), std::ios::binary);
    std::vector<uint8_t> payload((std::istreambuf_iterator<char>(pf)), {});
    ASSERT_EQ((int)payload.size(), ln);
    // Read IQ with matching length suffix
    auto iq_hdr = vec_dir / ("sf" + std::to_string(sf) + "_cr" + std::to_string(cr_int) + "_iq_os4_hdr_len" + std::to_string(ln) + ".bin");
    if (!fs::exists(iq_hdr)) continue;
    attempted++;
    std::ifstream iqf(iq_hdr, std::ios::binary);
    std::vector<std::complex<float>> iq; float buf[2];
    while (iqf.read(reinterpret_cast<char*>(buf), sizeof(buf))) iq.emplace_back(buf[0], buf[1]);
    auto out = loopback_rx_header_auto(ws, std::span<const std::complex<float>>(iq.data(), iq.size()),
                                       static_cast<uint32_t>(sf), cr_enum, 8, true);
    if (out.second) {
      EXPECT_EQ(out.first.size(), payload.size());
      EXPECT_TRUE(std::equal(out.first.begin(), out.first.end(), payload.begin()));
      decoded++;
    }
  }
  SUCCEED() << "Attempted=" << attempted << " decoded=" << decoded;
}

