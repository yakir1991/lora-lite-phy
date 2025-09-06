#include "lora/workspace.hpp"
#include "lora/tx/frame_tx.hpp"
#include "lora/rx/frame.hpp"
#include "lora/utils/gray.hpp"
#include <vector>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

static void usage(const char* a0) {
    std::fprintf(stderr, "Usage: %s --sf <7..12> --cr <45..48> --payload file.bin --out iq_f32.bin [--preamble 8] [--sync 0x34] [--os 1|2|4|8]\n", a0);
}

int main(int argc, char** argv) {
    int sf = 7; int cr_int = 45; const char* pay_path = nullptr; const char* out_path = nullptr;
    int pre = 8; unsigned int sync_hex = 0x34; int os = 4;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sf" && i+1 < argc) sf = std::stoi(argv[++i]);
        else if (a == "--cr" && i+1 < argc) cr_int = std::stoi(argv[++i]);
        else if (a == "--payload" && i+1 < argc) pay_path = argv[++i];
        else if (a == "--out" && i+1 < argc) out_path = argv[++i];
        else if (a == "--preamble" && i+1 < argc) pre = std::stoi(argv[++i]);
        else if (a == "--sync" && i+1 < argc) sync_hex = std::stoul(argv[++i], nullptr, 0);
        else if (a == "--os" && i+1 < argc) os = std::stoi(argv[++i]);
        else { usage(argv[0]); return 2; }
    }
    if (!pay_path || !out_path || sf < 7 || sf > 12 || cr_int < 45 || cr_int > 48) { usage(argv[0]); return 2; }

    std::ifstream f(pay_path, std::ios::binary); if (!f) { std::perror("payload"); return 1; }
    std::vector<uint8_t> payload((std::istreambuf_iterator<char>(f)), {});

    lora::utils::CodeRate cr = lora::utils::CodeRate::CR45;
    switch (cr_int) { case 45: cr = lora::utils::CodeRate::CR45; break; case 46: cr = lora::utils::CodeRate::CR46; break; case 47: cr = lora::utils::CodeRate::CR47; break; case 48: cr = lora::utils::CodeRate::CR48; break; }

    lora::Workspace ws; ws.init((uint32_t)sf);
    // Build local header
    lora::rx::LocalHeader hdr{ .payload_len = static_cast<uint8_t>(payload.size()), .cr = cr, .has_crc = true };
    // Modulate frame symbols (header+payload+crc) as upchirps with per-symbol shift
    auto iq_frame = lora::tx::frame_tx(ws, payload, (uint32_t)sf, cr, hdr);

    uint32_t N = ws.N;
    // Build full signal: preamble + sync + optional SFD + frame
    std::vector<std::complex<float>> sig; sig.reserve((pre + 3) * N + N/4 + iq_frame.size());
    for (int s = 0; s < pre; ++s)
        for (uint32_t n = 0; n < N; ++n) sig.push_back(ws.upchirp[n]);
    uint32_t sync_sym = lora::utils::gray_encode(sync_hex & 0xFFu) & (N - 1);
    for (uint32_t n = 0; n < N; ++n)
        sig.push_back(ws.upchirp[(n + sync_sym) % N]);
    // Standard LoRa SFD: two downchirps followed by a quarter upchirp
    for (int s = 0; s < 2; ++s)
        for (uint32_t n = 0; n < N; ++n) sig.push_back(ws.downchirp[n]);
    for (uint32_t n = 0; n < N/4; ++n) sig.push_back(ws.upchirp[n]);
    sig.insert(sig.end(), iq_frame.begin(), iq_frame.end());

    // Optional oversample by repeat
    std::vector<std::complex<float>> os_sig;
    if (os > 1) {
        os_sig.reserve(sig.size() * (size_t)os);
        for (auto v : sig) for (int i = 0; i < os; ++i) os_sig.push_back(v);
    }
    auto& out = (os > 1) ? os_sig : sig;

    std::ofstream o(out_path, std::ios::binary);
    if (!o) { std::perror("out"); return 1; }
    for (auto v : out) {
        float re = v.real(); float im = v.imag();
        o.write(reinterpret_cast<const char*>(&re), sizeof(float));
        o.write(reinterpret_cast<const char*>(&im), sizeof(float));
    }
    return 0;
}

