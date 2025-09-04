// Cross-validation test that compares local TX/RX against reference IQ and
// payload vectors produced by `scripts/export_vectors.sh`.
//
// Reference vectors contain floating-point IQ samples; equality checks allow
// for a small numeric tolerance to accommodate minor rounding differences.
#include "lora/rx/loopback_rx.hpp"
#include "lora/tx/loopback_tx.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace lora;
using namespace lora::tx;
using namespace lora::rx;
using namespace lora::utils;

TEST(ReferenceVectors, CrossValidate) {
  namespace fs = std::filesystem;
  Workspace ws;
  // Permissible numeric error when comparing reference and locally-generated
  // IQ samples.  A millesimal tolerance is sufficient for the deterministic
  // loopback implementation used to export vectors.
  const float kTol = 1e-3f;
  auto root = fs::path(__FILE__).parent_path().parent_path();
  auto vec_dir = root / "vectors";
  size_t tested = 0;
  for (const auto &entry : fs::directory_iterator(vec_dir)) {
    auto path = entry.path();
    auto name = path.filename().string();
    if (name.find("_payload.bin") == std::string::npos)
      continue;
    int sf = 0, cr_int = 0;
    if (sscanf(name.c_str(), "sf%d_cr%d_payload.bin", &sf, &cr_int) != 2)
      continue;
    CodeRate cr_enum;
    switch (cr_int) {
    case 45:
      cr_enum = CodeRate::CR45;
      break;
    case 46:
      cr_enum = CodeRate::CR46;
      break;
    case 47:
      cr_enum = CodeRate::CR47;
      break;
    case 48:
      cr_enum = CodeRate::CR48;
      break;
    default:
      continue;
    }
    std::ifstream pf(path, std::ios::binary);
    std::vector<uint8_t> payload((std::istreambuf_iterator<char>(pf)), {});
    std::string iq_name = (vec_dir / ("sf" + std::to_string(sf) + "_cr" +
                                      std::to_string(cr_int) + "_iq.bin"))
                              .string();
    std::ifstream iqf(iq_name, std::ios::binary);
    std::vector<std::complex<float>> iq_full;
    float buf[2];
    while (iqf.read(reinterpret_cast<char *>(buf), sizeof(buf)))
      iq_full.emplace_back(buf[0], buf[1]);
    SCOPED_TRACE(name);

    // Try to handle reference IQ that may include preamble/header and OS>1.
    const std::vector<int> os_candidates = {1, 2, 4, 8};
    const uint32_t N = 1u << sf;
    const uint32_t cr_plus4 = static_cast<uint32_t>(cr_enum) + 4u;
    const size_t bits_needed = (payload.size() + 2u) * 2u * cr_plus4;
    const size_t min_syms = (bits_needed + sf - 1u) / sf; // ceil

    bool decoded = false;
    size_t best_start_sym = 0;
    int used_os = 1;
    std::vector<std::complex<float>> iq_decim;
    for (int os : os_candidates) {
      if (iq_full.size() < os)
        continue;
      iq_decim.clear();
      iq_decim.reserve(iq_full.size() / os + 1);
      for (size_t i = 0; i < iq_full.size(); i += os)
        iq_decim.push_back(iq_full[i]);
      if (iq_decim.size() < N * min_syms)
        continue;
      size_t nsym_total = iq_decim.size() / N;
      // Slide window over symbol boundaries and try to decode
      for (size_t start_sym = 0; start_sym + min_syms <= nsym_total; ++start_sym) {
        auto start_idx = start_sym * N;
        std::span<const std::complex<float>> win(&iq_decim[start_idx], iq_decim.size() - start_idx);
        auto rx = lora::rx::loopback_rx(ws, win, sf, cr_enum, payload.size());
        if (rx.second && rx.first.size() == payload.size() &&
            std::equal(rx.first.begin(), rx.first.end(), payload.begin())) {
          decoded = true;
          best_start_sym = start_sym;
          used_os = os;
          break;
        }
      }
      if (decoded) break;
    }
    ASSERT_TRUE(decoded) << "Failed to locate payload in reference IQ (preamble/header or OS mismatch)";

    // Correlate local TX IQ against the aligned reference slice
    auto tx_iq = lora::tx::loopback_tx(ws, payload, sf, cr_enum);
    size_t start_idx = best_start_sym * N;
    size_t L = std::min(tx_iq.size(), iq_decim.size() - start_idx);
    ASSERT_GT(L, 0u);
    std::complex<float> cross{0.0f, 0.0f};
    float ref_energy = 0.0f;
    float tx_energy = 0.0f;
    for (size_t i = 0; i < L; ++i) {
      cross += tx_iq[i] * std::conj(iq_decim[start_idx + i]);
      ref_energy += std::norm(iq_decim[start_idx + i]);
      tx_energy += std::norm(tx_iq[i]);
    }
    auto alpha = cross / ref_energy;
    float corr = std::abs(cross) / std::sqrt(ref_energy * tx_energy);
    EXPECT_NEAR(corr, 1.0f, kTol) << "OS=" << used_os << ", start_sym=" << best_start_sym;
    for (size_t i = 0; i < L; ++i) {
      auto adj = tx_iq[i] / alpha;
      EXPECT_NEAR(adj.real(), iq_decim[start_idx + i].real(), kTol);
      EXPECT_NEAR(adj.imag(), iq_decim[start_idx + i].imag(), kTol);
    }
    ++tested;
  }
  // Expect at least one reference vector pair to be present.
  ASSERT_GE(tested, 2u);
}
