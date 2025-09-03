#include "lora/rx/loopback_rx.hpp"
#include "lora/tx/loopback_tx.hpp"
#include "lora/workspace.hpp"
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <span>

using namespace lora;
using namespace lora::utils;

struct SweepConfig {
    float snr_min = -20.f;
    float snr_max = 10.f;
    float snr_step = 1.f;
    int   frames = 500;
    uint32_t sf = 7;
    uint32_t seed = 123;
    bool impair = true;
    float cfo_hz = 100.f;
    float sfo_ppm = 25.f;
    float jitter = 0.02f;
    float mf_alpha = 0.10f;
    float fs_hz = 125000.f;
    std::string csv = "awgn_sweep.csv";
};

static SweepConfig g_cfg;

static std::string cr_to_string(CodeRate cr) {
    switch (cr) {
    case CodeRate::CR45: return "4/5";
    case CodeRate::CR46: return "4/6";
    case CodeRate::CR47: return "4/7";
    case CodeRate::CR48: return "4/8";
    }
    return "";
}

static std::complex<float> interp(const std::vector<std::complex<float>>& v,
                                  float idx) {
    if (idx <= 0)
        return v.front();
    float max_idx = static_cast<float>(v.size() - 1);
    if (idx >= max_idx)
        return v.back();
    size_t i0 = static_cast<size_t>(idx);
    float frac = idx - static_cast<float>(i0);
    return v[i0] + (v[i0 + 1] - v[i0]) * frac;
}

static std::vector<std::complex<float>>
apply_impairments(std::span<const std::complex<float>> in, std::mt19937& rng) {
    std::vector<std::complex<float>> sig(in.begin(), in.end());

    // CFO
    if (g_cfg.cfo_hz != 0.f) {
        float phase_inc = 2.f * float(M_PI) * g_cfg.cfo_hz / g_cfg.fs_hz;
        for (size_t i = 0; i < sig.size(); ++i) {
            float phi = phase_inc * static_cast<float>(i);
            sig[i] *= std::complex<float>(std::cos(phi), std::sin(phi));
        }
    }

    // SFO
    if (g_cfg.sfo_ppm != 0.f) {
        float sfo = g_cfg.sfo_ppm * 1e-6f;
        std::vector<std::complex<float>> tmp(sig.size());
        for (size_t i = 0; i < sig.size(); ++i) {
            float t = static_cast<float>(i) * (1.f + sfo);
            tmp[i] = interp(sig, t);
        }
        sig.swap(tmp);
    }

    // Timing jitter
    if (g_cfg.jitter > 0.f) {
        std::normal_distribution<float> jdist(0.f, g_cfg.jitter);
        std::vector<std::complex<float>> tmp(sig.size());
        for (size_t i = 0; i < sig.size(); ++i) {
            float t = static_cast<float>(i) + jdist(rng);
            tmp[i] = interp(sig, t);
        }
        sig.swap(tmp);
    }

    // Simple IIR filter mismatch
    if (g_cfg.mf_alpha > 0.f) {
        std::vector<std::complex<float>> tmp(sig.size());
        if (!sig.empty()) {
            tmp[0] = sig[0];
            for (size_t i = 1; i < sig.size(); ++i) {
                tmp[i] = g_cfg.mf_alpha * sig[i] +
                         (1.f - g_cfg.mf_alpha) * tmp[i - 1];
            }
        }
        sig.swap(tmp);
    }

    return sig;
}

// Parse environment variables
static void apply_env(SweepConfig& cfg) {
    auto get_env = [](const char* name) -> const char* { return std::getenv(name); };
    if (auto v = get_env("AWGN_SNR_MIN")) cfg.snr_min = std::atof(v);
    if (auto v = get_env("AWGN_SNR_MAX")) cfg.snr_max = std::atof(v);
    if (auto v = get_env("AWGN_SNR_STEP")) cfg.snr_step = std::atof(v);
    if (auto v = get_env("AWGN_FRAMES")) cfg.frames = std::atoi(v);
    if (auto v = get_env("AWGN_SF")) cfg.sf = static_cast<uint32_t>(std::atoi(v));
    if (auto v = get_env("AWGN_SEED")) cfg.seed = static_cast<uint32_t>(std::atoi(v));
    if (auto v = get_env("AWGN_IMPAIR")) cfg.impair = std::atoi(v) != 0;
    if (auto v = get_env("AWGN_CFO_HZ")) cfg.cfo_hz = std::atof(v);
    if (auto v = get_env("AWGN_SFO_PPM")) cfg.sfo_ppm = std::atof(v);
    if (auto v = get_env("AWGN_JITTER")) cfg.jitter = std::atof(v);
    if (auto v = get_env("AWGN_MF_ALPHA")) cfg.mf_alpha = std::atof(v);
    if (auto v = get_env("AWGN_FS_HZ")) cfg.fs_hz = std::atof(v);
    if (auto v = get_env("AWGN_CSV")) cfg.csv = v;
}

// Parse CLI options and strip them from argv so gtest doesn't see them
static void parse_cli(int& argc, char** argv, SweepConfig& cfg) {
    std::vector<char*> gtest_args;
    gtest_args.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        auto next = [&](float& dst) { dst = std::atof(argv[++i]); };
        auto next_int = [&](int& dst) { dst = std::atoi(argv[++i]); };
        auto next_u32 = [&](uint32_t& dst) { dst = static_cast<uint32_t>(std::atoi(argv[++i])); };
        auto next_str = [&](std::string& dst) { dst = argv[++i]; };
        if (strcmp(argv[i], "--snr-min") == 0) next(cfg.snr_min);
        else if (strcmp(argv[i], "--snr-max") == 0) next(cfg.snr_max);
        else if (strcmp(argv[i], "--snr-step") == 0) next(cfg.snr_step);
        else if (strcmp(argv[i], "--frames") == 0) next_int(cfg.frames);
        else if (strcmp(argv[i], "--sf") == 0) next_u32(cfg.sf);
        else if (strcmp(argv[i], "--seed") == 0) next_u32(cfg.seed);
        else if (strcmp(argv[i], "--impair") == 0) { int v; next_int(v); cfg.impair = v != 0; }
        else if (strcmp(argv[i], "--cfo-hz") == 0) next(cfg.cfo_hz);
        else if (strcmp(argv[i], "--sfo-ppm") == 0) next(cfg.sfo_ppm);
        else if (strcmp(argv[i], "--jitter") == 0) next(cfg.jitter);
        else if (strcmp(argv[i], "--mf-alpha") == 0) next(cfg.mf_alpha);
        else if (strcmp(argv[i], "--fs-hz") == 0) next(cfg.fs_hz);
        else if (strcmp(argv[i], "--csv") == 0) next_str(cfg.csv);
        else
            gtest_args.push_back(argv[i]);
    }
    argc = static_cast<int>(gtest_args.size());
    for (int i = 0; i < argc; ++i)
        argv[i] = gtest_args[i];
}

TEST(AWGN, SNR_Sweep) {
    Workspace ws;
    std::mt19937 rng(g_cfg.seed);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    const size_t payload_len = 16;

    std::ofstream csv(g_cfg.csv);
    csv << "snr_db,cr,frames,frame_errors,fer,bit_errors,ber\n";

    for (float snr_db = g_cfg.snr_min; snr_db <= g_cfg.snr_max + 1e-6f;
         snr_db += g_cfg.snr_step) {
        for (CodeRate cr : {CodeRate::CR45, CodeRate::CR46,
                            CodeRate::CR47, CodeRate::CR48}) {
            size_t frame_errors = 0;
            size_t bit_errors = 0;
            size_t total_bits = payload_len * 8 * static_cast<size_t>(g_cfg.frames);

            for (int f = 0; f < g_cfg.frames; ++f) {
                std::vector<uint8_t> payload(payload_len);
                for (auto& b : payload)
                    b = static_cast<uint8_t>(byte_dist(rng));

                auto txsig = tx::loopback_tx(ws, payload, g_cfg.sf, cr);
                auto impair_sig = g_cfg.impair ? apply_impairments(txsig, rng) : txsig;

                float P = 0.f;
                for (auto& s : impair_sig)
                    P += std::norm(s);
                P /= static_cast<float>(impair_sig.size());

                float snr_lin = std::pow(10.f, snr_db / 10.f);
                float noise_var = P / snr_lin;
                std::normal_distribution<float> noise(0.f, std::sqrt(noise_var / 2.f));
                std::vector<std::complex<float>> noisy(impair_sig.size());
                for (size_t i = 0; i < impair_sig.size(); ++i)
                    noisy[i] = impair_sig[i] +
                               std::complex<float>(noise(rng), noise(rng));

                auto rxres = rx::loopback_rx(ws, noisy, g_cfg.sf, cr, payload.size());
                bool frame_ok = rxres.second && rxres.first == payload;
                if (!frame_ok)
                    frame_errors++;
                if (rxres.second && rxres.first.size() == payload.size()) {
                    for (size_t i = 0; i < payload.size(); ++i)
                        bit_errors += static_cast<size_t>(
                            __builtin_popcount(payload[i] ^ rxres.first[i]));
                } else {
                    bit_errors += payload.size() * 8;
                }
            }

            double fer = static_cast<double>(frame_errors) /
                         static_cast<double>(g_cfg.frames);
            double ber = static_cast<double>(bit_errors) /
                         static_cast<double>(total_bits);

            std::cout << "SNR " << snr_db << " dB  CR " << cr_to_string(cr)
                      << "  Frames " << g_cfg.frames
                      << "  FER " << fer
                      << "  BER " << ber
                      << "  (FErr=" << frame_errors
                      << ", BErr=" << bit_errors << ")" << std::endl;

            csv << snr_db << ',' << cr_to_string(cr) << ',' << g_cfg.frames << ','
                << frame_errors << ',' << fer << ','
                << bit_errors << ',' << ber << '\n';
        }
    }
}

int main(int argc, char** argv) {
    apply_env(g_cfg);
    parse_cli(argc, argv, g_cfg);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

