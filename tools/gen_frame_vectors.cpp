#include "lora/tx/frame_tx.hpp"
#include "lora/utils/gray.hpp"
#include "lora/constants.hpp"
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace lora;
using namespace lora::utils;

static void usage(const char* a0) {
    std::cerr << "Usage: " << a0 << " --sf <7..12> --cr <45|46|47|48> --payload <file> --out <iq_file> [--os 1|2|4|8] [--preamble 8]\n";
}

static std::vector<std::complex<float>> upsample_repeat(std::span<const std::complex<float>> x, int os) {
    if (os <= 1) return std::vector<std::complex<float>>(x.begin(), x.end());
    std::vector<std::complex<float>> y;
    y.reserve(x.size() * os);
    for (auto v : x) for (int i = 0; i < os; ++i) y.push_back(v);
    return y;
}

int main(int argc, char** argv) {
    int sf = 0; int cr_int = 45; int os = 1; int pre_len = 8; std::string payload_path, out_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sf" && i + 1 < argc) sf = std::stoi(argv[++i]);
        else if (a == "--cr" && i + 1 < argc) cr_int = std::stoi(argv[++i]);
        else if (a == "--payload" && i + 1 < argc) payload_path = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--os" && i + 1 < argc) os = std::stoi(argv[++i]);
        else if (a == "--preamble" && i + 1 < argc) pre_len = std::stoi(argv[++i]);
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
    // Build preamble + sync + frame
    std::vector<std::complex<float>> sig;
    sig.resize((pre_len + 1) * N + iq_frame.size());
    for (int s = 0; s < pre_len; ++s)
        for (uint32_t n = 0; n < N; ++n) sig[s * N + n] = ws.upchirp[n];
    uint32_t sync_sym = lora::utils::gray_encode(static_cast<uint32_t>(lora::LORA_SYNC_WORD_PUBLIC));
    for (uint32_t n = 0; n < N; ++n) sig[pre_len * N + n] = ws.upchirp[(n + sync_sym) % N];
    std::copy(iq_frame.begin(), iq_frame.end(), sig.begin() + (pre_len + 1) * N);
    // Upsample if needed
    auto sig_os = upsample_repeat(std::span<const std::complex<float>>(sig.data(), sig.size()), os);
    // Write float32 IQ
    std::ofstream of(out_path, std::ios::binary); if (!of) { std::cerr << "out open failed\n"; return 1; }
    for (auto c : sig_os) { float re = c.real(), im = c.imag(); of.write(reinterpret_cast<const char*>(&re), sizeof(float)); of.write(reinterpret_cast<const char*>(&im), sizeof(float)); }
    return 0;
}
