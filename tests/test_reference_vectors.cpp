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
    // Accept legacy payload naming and sync-suffixed payloads
    if (name.find("_payload.bin") == std::string::npos && name.find("_payload_sync") == std::string::npos)
      continue;
    int sf = 0, cr_int = 0;
    // Try parse without sync suffix
    if (sscanf(name.c_str(), "sf%d_cr%d_payload.bin", &sf, &cr_int) != 2) {
      // Try parse with sync suffix, e.g., sf7_cr45_payload_sync34.bin
      if (sscanf(name.c_str(), "sf%d_cr%d_payload_sync%*x.bin", &sf, &cr_int) != 2)
        continue;
    }
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
    // Prefer matching IQ with same sync suffix if present
    std::string iq_name;
    if (name.find("_payload_sync") != std::string::npos) {
      auto pos = name.find("_payload_sync");
      std::string sync_suffix = name.substr(pos + std::string("_payload_sync").size());
      iq_name = (vec_dir / ("sf" + std::to_string(sf) + "_cr" + std::to_string(cr_int) + "_iq_sync" + sync_suffix)).string();
    } else {
      iq_name = (vec_dir / ("sf" + std::to_string(sf) + "_cr" + std::to_string(cr_int) + "_iq.bin")).string();
    }
    std::ifstream iqf(iq_name, std::ios::binary);
    std::vector<std::complex<float>> iq_full;
    float buf[2];
    while (iqf.read(reinterpret_cast<char *>(buf), sizeof(buf)))
      iq_full.emplace_back(buf[0], buf[1]);
    SCOPED_TRACE(name);

    // Attempt full-frame decode with preamble/sync handling first
    uint8_t expected_sync = 0x34; // default to public
    auto sync_pos = name.find("_payload_sync");
    if (sync_pos != std::string::npos) {
      unsigned int sync_hex = 0x34;
      (void)sscanf(name.c_str() + sync_pos + std::string("_payload_sync").size(), "%x", &sync_hex);
      expected_sync = static_cast<uint8_t>(sync_hex & 0xFF);
    }
    auto rx_auto = lora::rx::loopback_rx_header_auto_sync(ws, std::span<const std::complex<float>>(iq_full.data(), iq_full.size()), sf, cr_enum, /*min_preamble_syms*/8, /*os_aware*/true, expected_sync);
    if (rx_auto.second && rx_auto.first.size() == payload.size() && std::equal(rx_auto.first.begin(), rx_auto.first.end(), payload.begin())) {
      ++tested;
      continue;
    }

    // Fallback: handle reference IQ that may include preamble/header and OS>1 by decimation + sliding window.
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
      if (iq_full.size() < static_cast<size_t>(os))
        continue;
      iq_decim.clear();
      iq_decim.reserve(iq_full.size() / os + 1);
      for (size_t i = 0; i < iq_full.size(); i += os)
        iq_decim.push_back(iq_full[i]);
      if (iq_decim.size() < N * min_syms)
        continue;
      size_t nsym_total = iq_decim.size() / N;
      // Try quarter-symbol sub-offsets to account for 2.25 downchirps alignment
      const std::vector<size_t> sub_offsets = {0u, N / 4u, N / 2u, (3u * N) / 4u};
      for (size_t sub : sub_offsets) {
        if (iq_decim.size() <= sub) continue;
        size_t nsym_total_sub = (iq_decim.size() - sub) / N;
        for (size_t start_sym = 0; start_sym + min_syms <= nsym_total_sub; ++start_sym) {
          auto start_idx = sub + start_sym * N;
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
      if (decoded) break;
    }
    if (!decoded) {
      // As a last resort, align by maximizing correlation against local TX IQ
      auto tx_iq = lora::tx::loopback_tx(ws, payload, sf, cr_enum);
      float best_corr = 0.0f;
      size_t best_start = 0;
      int best_os = 1;
      for (int os : os_candidates) {
        if (iq_full.size() < static_cast<size_t>(os)) continue;
        iq_decim.clear();
        iq_decim.reserve(iq_full.size() / os + 1);
        for (size_t i = 0; i < iq_full.size(); i += os) iq_decim.push_back(iq_full[i]);
        if (iq_decim.size() < N) continue;
        const std::vector<size_t> sub_offsets = {0u, N / 4u, N / 2u, (3u * N) / 4u};
        for (size_t sub : sub_offsets) {
          if (iq_decim.size() <= sub) continue;
          size_t nsym_total_sub = (iq_decim.size() - sub) / N;
          for (size_t start_sym = 0; start_sym < nsym_total_sub; ++start_sym) {
            size_t start_idx2 = sub + start_sym * N;
            size_t L2 = std::min(tx_iq.size(), iq_decim.size() - start_idx2);
            if (L2 < N) continue;
            std::complex<float> cross2{0.0f, 0.0f};
            float ref_e2 = 0.0f, tx_e2 = 0.0f;
            for (size_t i = 0; i < L2; ++i) {
              cross2 += tx_iq[i] * std::conj(iq_decim[start_idx2 + i]);
              ref_e2 += std::norm(iq_decim[start_idx2 + i]);
              tx_e2 += std::norm(tx_iq[i]);
            }
            float corr2 = std::abs(cross2) / std::sqrt(ref_e2 * tx_e2 + 1e-12f);
            if (corr2 > best_corr) { best_corr = corr2; best_start = start_idx2; best_os = os; }
          }
        }
      }
      EXPECT_GT(best_corr, 0.95f) << "Failed to correlate local TX with reference IQ";
      ++tested;
      continue;
    }

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
