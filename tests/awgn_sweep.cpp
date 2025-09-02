#include "lora/rx/loopback_rx.hpp"
#include "lora/tx/loopback_tx.hpp"
#include "lora/workspace.hpp"
#include <cmath>
#include <gtest/gtest.h>
#include <iostream>
#include <random>

using namespace lora;
using namespace lora::utils;

struct SweepResult {
  float snr_db;
  CodeRate cr;
  double ber;
  double fer;
};

TEST(AWGN, SNR_Sweep) {
  Workspace ws;
  std::mt19937 rng(123);
  std::uniform_int_distribution<int> byte_dist(0, 255);

  const uint32_t sf = 7;
  const size_t payload_len = 16;
  const int num_payloads = 10;

  std::vector<float> snr_values = {0.f, 5.f, 10.f, 15.f, 20.f};
  std::vector<CodeRate> cr_values = {CodeRate::CR45, CodeRate::CR46,
                                     CodeRate::CR47, CodeRate::CR48};
  std::vector<SweepResult> results;

  std::vector<uint8_t> payload(payload_len);

  for (float snr_db : snr_values) {
    float snr_lin = std::pow(10.0f, snr_db / 10.0f);
    float sigma = std::sqrt(1.0f / (2.0f * snr_lin));
    std::normal_distribution<float> noise(0.0f, sigma);

    for (CodeRate cr : cr_values) {
      size_t bit_errors = 0;
      size_t frame_errors = 0;
      size_t total_bits = payload_len * 8 * num_payloads;

      for (int p = 0; p < num_payloads; ++p) {
        for (auto &b : payload)
          b = byte_dist(rng);
        auto txsig = tx::loopback_tx(ws, payload, sf, cr);
        auto noisy = txsig;
        for (auto &s : noisy) {
          s += std::complex<float>(noise(rng), noise(rng));
        }
        auto rxres = rx::loopback_rx(ws, noisy, sf, cr, payload.size());

        bool frame_ok = rxres.second && rxres.first == payload;
        if (!frame_ok) {
          frame_errors++;
        }
        if (rxres.second && rxres.first.size() == payload.size()) {
          for (size_t i = 0; i < payload.size(); ++i) {
            bit_errors += static_cast<size_t>(
                __builtin_popcount(payload[i] ^ rxres.first[i]));
          }
        } else {
          bit_errors += payload.size() * 8;
        }
      }
      double ber =
          static_cast<double>(bit_errors) / static_cast<double>(total_bits);
      double fer =
          static_cast<double>(frame_errors) / static_cast<double>(num_payloads);
      results.push_back({snr_db, cr, ber, fer});
    }
  }

  for (const auto &r : results) {
    std::cout << "SNR " << r.snr_db << " dB CR 4/"
              << (static_cast<int>(r.cr) + 4) << " BER " << r.ber << " FER "
              << r.fer << std::endl;
  }

  SUCCEED();
}
