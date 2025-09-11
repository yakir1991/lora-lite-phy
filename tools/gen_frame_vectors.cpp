#include "lora/tx/frame_tx.hpp"
#include "lora/utils/gray.hpp"
#include "lora/constants.hpp"
#include <liquid/liquid.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace lora;
using namespace lora::utils;

static void usage(const char* a0) {
    std::cerr << "Usage: " << a0 << " --sf <7..12> --cr <45|46|47|48> --payload <file> --out <iq_file> [--os 1|2|4|8] [--preamble 8] [--interp repeat|poly]\n";
}

// Repeat (zero-order hold) upsample for quick synthetic OS>1
static std::vector<std::complex<float>> upsample_repeat(std::span<const std::complex<float>> x, int os) {
    if (os <= 1) return std::vector<std::complex<float>>(x.begin(), x.end());
    std::vector<std::complex<float>> y;
    y.reserve(x.size() * static_cast<size_t>(os));
    for (auto v : x) for (int i = 0; i < os; ++i) y.push_back(v);
    return y;
}

// Polyphase interpolation by integer factor using Liquid-DSP firinterp_crcf
static std::vector<std::complex<float>> upsample_polyphase(std::span<const std::complex<float>> x, int os, float as_db = 60.0f) {
    if (os <= 1) return std::vector<std::complex<float>>(x.begin(), x.end());
    const unsigned int M = static_cast<unsigned int>(os);
    const float fc = 0.45f / static_cast<float>(os);
    const unsigned int L = std::max<unsigned int>(32u * M, 8u * M);
    std::vector<float> h(L);
    liquid_firdes_kaiser(L, fc, as_db, 0.0f, h.data());
    firinterp_crcf q = firinterp_crcf_create(M, h.data(), L);
    std::vector<std::complex<float>> y; y.resize(x.size() * static_cast<size_t>(os));
    size_t yo = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        liquid_float_complex outbuf[64];
        liquid_float_complex xi = *reinterpret_cast<const liquid_float_complex*>(&x[i]);
        firinterp_crcf_execute(q, xi, outbuf);
        for (unsigned int k = 0; k < M; ++k) y[yo++] = *reinterpret_cast<std::complex<float>*>(&outbuf[k]);
    }
    firinterp_crcf_destroy(q);
    return y;
}

int main(int argc, char** argv) {
    int sf = 0; int cr_int = 45; int os = 1; int pre_len = 8; std::string payload_path, out_path; std::string interp = "poly";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sf" && i + 1 < argc) sf = std::stoi(argv[++i]);
        else if (a == "--cr" && i + 1 < argc) cr_int = std::stoi(argv[++i]);
        else if (a == "--payload" && i + 1 < argc) payload_path = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--os" && i + 1 < argc) os = std::stoi(argv[++i]);
        else if (a == "--preamble" && i + 1 < argc) pre_len = std::stoi(argv[++i]);
        else if (a == "--interp" && i + 1 < argc) interp = argv[++i];
        else { usage(argv[0]); return 1; }
    }
    if (sf < 7 || sf > 12 || cr_int < 45 || cr_int > 48 || payload_path.empty() || out_path.empty()) { usage(argv[0]); return 1; }
    CodeRate cr = CodeRate::CR45;
    switch (cr_int) { case 45: cr = CodeRate::CR45; break; case 46: cr = CodeRate::CR46; break; case 47: cr = CodeRate::CR47; break; case 48: cr = CodeRate::CR48; break; default: return 1; }
    std::ifstream pf(payload_path, std::ios::binary); if (!pf) { std::cerr << "payload open failed\n"; return 1; }
    std::vector<uint8_t> payload((std::istreambuf_iterator<char>(pf)), {});
    lora::rx::LocalHeader hdr; hdr.payload_len = static_cast<uint8_t>(payload.size()); hdr.cr = cr; hdr.has_crc = true;
    Workspace ws;
    auto iq_frame = lora::tx::frame_tx(ws, payload, static_cast<uint32_t>(sf), cr, hdr);
    ws.init(sf);
    uint32_t N = ws.N;
    // Build preamble + sync (two upchirps at hi<<3 and lo<<3) + two downchirps + frame
    std::vector<std::complex<float>> sig;
    const uint8_t sync = static_cast<uint8_t>(lora::LORA_SYNC_WORD_PUBLIC & 0xFF);
    const uint32_t net1 = ((sync & 0xF0u) >> 4) << 3; // hi nibble << 3
    const uint32_t net2 = (sync & 0x0Fu) << 3;        // lo nibble << 3
    const int sync_syms = 4; // two up + two down
    // Reserve space for preamble + sync (2 up) + 2 down + quarter upchirp + frame
    sig.resize((pre_len + sync_syms) * N + N/4 + iq_frame.size());
    // Preamble (upchirps)
    for (int s = 0; s < pre_len; ++s)
        for (uint32_t n = 0; n < N; ++n) sig[s * N + n] = ws.upchirp[n];
    // Sync upchirps: hi then lo
    for (uint32_t n = 0; n < N; ++n) sig[(pre_len + 0) * N + n] = ws.upchirp[(n + net1) % N];
    for (uint32_t n = 0; n < N; ++n) sig[(pre_len + 1) * N + n] = ws.upchirp[(n + net2) % N];
    // Two downchirps after sync (SFD-like)
    for (uint32_t n = 0; n < N; ++n) sig[(pre_len + 2) * N + n] = ws.downchirp[n];
    for (uint32_t n = 0; n < N; ++n) sig[(pre_len + 3) * N + n] = ws.downchirp[n];
    // Quarter upchirp tail for SFD (improves header anchor alignment at sync + 2.25 symbols)
    for (uint32_t n = 0; n < N/4; ++n) sig[(pre_len + sync_syms) * N + n] = ws.upchirp[n];
    // Append frame after sync + downchirps + quarter upchirp
    std::copy(iq_frame.begin(), iq_frame.end(), sig.begin() + (pre_len + sync_syms) * N + N/4);
    // Upsample if needed (select method)
    std::vector<std::complex<float>> sig_os;
    if (interp == "repeat") sig_os = upsample_repeat(std::span<const std::complex<float>>(sig.data(), sig.size()), os);
    else sig_os = upsample_polyphase(std::span<const std::complex<float>>(sig.data(), sig.size()), os);
    // Write float32 IQ
    std::ofstream of(out_path, std::ios::binary); if (!of) { std::cerr << "out open failed\n"; return 1; }
    for (auto c : sig_os) { float re = c.real(), im = c.imag(); of.write(reinterpret_cast<const char*>(&re), sizeof(float)); of.write(reinterpret_cast<const char*>(&im), sizeof(float)); }
    // Debug: print GR-direct header bytes from first 10 header symbols (5 bytes)
    if (ws.tx_symbols.size() >= 10) {
        std::vector<uint8_t> nibb; nibb.reserve(10);
        for (size_t s = 0; s < 10; ++s) {
            uint32_t raw = ws.tx_symbols[s] & (N - 1);
            uint32_t corr = (raw + N - 44u) % N;
            uint32_t g = lora::utils::gray_encode(corr);
            uint32_t gnu = ((g + (1u << sf) - 1u) & (N - 1u)) >> 2u;
            nibb.push_back(static_cast<uint8_t>(gnu & 0x0F));
        }
        uint8_t b[5]{};
        for (int i = 0; i < 5; ++i) b[i] = static_cast<uint8_t>((nibb[2*i+1] << 4) | nibb[2*i]);
        std::fprintf(stderr, "[gen] TX GR-direct header bytes: %02x %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3], b[4]);
    }
    return 0;
}
