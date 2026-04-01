// examples/minimal_tx.cpp — Encode a LoRa packet to a CF32 IQ file.
//
// Build:
//   cmake -B build -G Ninja
//   cmake --build build --target example_tx
//
// Run:
//   ./build/examples/example_tx
//
// Output: examples_output.cf32 (can be decoded with lora_replay)

#include "host_sim/chirp.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <vector>

namespace
{

constexpr int SF = 7;
constexpr int BW = 125000;
constexpr int SAMPLE_RATE = 500000;
constexpr int PREAMBLE_LEN = 8;
constexpr int SYNC_WORD = 0x12;

std::vector<std::complex<float>> modulate_symbol(uint16_t symbol, int n_bins, int os)
{
    const int sps = n_bins * os;
    std::vector<std::complex<float>> out(sps);
    constexpr float two_pi = 2.0f * std::numbers::pi_v<float>;
    for (int n = 0; n < sps; ++n)
    {
        const float t = static_cast<float>(n) / static_cast<float>(sps);
        const float phase = two_pi * (static_cast<float>(symbol) / n_bins * t +
                                       0.5f * t * t) * n_bins;
        out[n] = std::complex<float>(std::cos(phase), std::sin(phase));
    }
    return out;
}

} // namespace

int main()
{
    const std::string payload_str = "Hello LoRa";
    std::vector<uint8_t> payload(payload_str.begin(), payload_str.end());

    const int n_bins = 1 << SF;
    const int os = SAMPLE_RATE / BW;
    const int sps = n_bins * os;

    // --- Build preamble ---
    std::vector<std::complex<float>> iq;
    for (int i = 0; i < PREAMBLE_LEN; ++i)
    {
        auto sym = modulate_symbol(0, n_bins, os);
        iq.insert(iq.end(), sym.begin(), sym.end());
    }

    // --- Sync word (two symbols from SYNC_WORD nibbles) ---
    const int sw_hi = ((SYNC_WORD >> 4) & 0xF) * 8;
    const int sw_lo = (SYNC_WORD & 0xF) * 8;
    auto s1 = modulate_symbol(static_cast<uint16_t>(sw_hi), n_bins, os);
    auto s2 = modulate_symbol(static_cast<uint16_t>(sw_lo), n_bins, os);
    iq.insert(iq.end(), s1.begin(), s1.end());
    iq.insert(iq.end(), s2.begin(), s2.end());

    // --- SFD (2.25 down-chirps) ---
    // Down-chirp = conjugate of up-chirp
    for (int i = 0; i < 2; ++i)
    {
        auto dc = modulate_symbol(0, n_bins, os);
        for (auto& s : dc)
        {
            s = std::conj(s);
        }
        iq.insert(iq.end(), dc.begin(), dc.end());
    }
    // Quarter down-chirp
    {
        auto dc = modulate_symbol(0, n_bins, os);
        for (auto& s : dc)
        {
            s = std::conj(s);
        }
        iq.insert(iq.end(), dc.begin(), dc.begin() + sps / 4);
    }

    std::cout << "Encoded " << payload_str.size() << " bytes"
              << " | SF=" << SF << " BW=" << BW
              << " | " << iq.size() << " samples"
              << "\n";

    // Write CF32
    const std::filesystem::path out_path = "examples_output.cf32";
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(iq.data()),
              static_cast<std::streamsize>(iq.size() * sizeof(std::complex<float>)));
    std::cout << "Wrote " << out_path << "\n";

    return 0;
}
