#include "lora/tx/loopback_tx.hpp"
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace lora;
using namespace lora::utils;

static void usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " --sf <7..12> --cr <45|46|47|48> --payload <file> --out <iq_file>\n";
}

int main(int argc, char** argv) {
    int sf = 0;
    int cr_int = 45;
    std::string payload_path;
    std::string out_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sf" && i + 1 < argc) { sf = std::stoi(argv[++i]); }
        else if (a == "--cr" && i + 1 < argc) { cr_int = std::stoi(argv[++i]); }
        else if (a == "--payload" && i + 1 < argc) { payload_path = argv[++i]; }
        else if (a == "--out" && i + 1 < argc) { out_path = argv[++i]; }
        else { usage(argv[0]); return 1; }
    }
    if (sf < 7 || sf > 12 || (cr_int < 45 || cr_int > 48) || payload_path.empty() || out_path.empty()) {
        usage(argv[0]);
        return 1;
    }
    CodeRate cr = CodeRate::CR45;
    switch (cr_int) {
        case 45: cr = CodeRate::CR45; break;
        case 46: cr = CodeRate::CR46; break;
        case 47: cr = CodeRate::CR47; break;
        case 48: cr = CodeRate::CR48; break;
        default: usage(argv[0]); return 1;
    }
    std::ifstream pf(payload_path, std::ios::binary);
    if (!pf) { std::cerr << "Failed to open payload\n"; return 1; }
    std::vector<uint8_t> payload((std::istreambuf_iterator<char>(pf)), {});

    Workspace ws;
    auto iq = lora::tx::loopback_tx(ws, payload, static_cast<uint32_t>(sf), cr);
    std::ofstream of(out_path, std::ios::binary);
    if (!of) { std::cerr << "Failed to open output\n"; return 1; }
    for (auto c : iq) {
        float re = c.real();
        float im = c.imag();
        of.write(reinterpret_cast<const char*>(&re), sizeof(float));
        of.write(reinterpret_cast<const char*>(&im), sizeof(float));
    }
    return 0;
}

