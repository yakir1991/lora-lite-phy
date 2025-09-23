#include "state_machine.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace lora::standalone;

static bool read_iq_f32(const char* path, std::vector<std::complex<float>>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    size_t count = static_cast<size_t>(sz) / (2*sizeof(float));
    std::vector<float> buf(count*2);
    f.read(reinterpret_cast<char*>(buf.data()), buf.size()*sizeof(float));
    out.resize(count);
    for (size_t i = 0; i < count; ++i) out[i] = {buf[2*i+0], buf[2*i+1]};
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <iq_f32_file> [sf=7] [bw=125000] [fs=250000]\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];
    uint32_t sf = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 7u;
    uint32_t bw = (argc > 3) ? static_cast<uint32_t>(std::atoi(argv[3])) : 125000u;
    uint32_t fs = (argc > 4) ? static_cast<uint32_t>(std::atoi(argv[4])) : 250000u;
    uint32_t os = fs / bw;

    std::vector<std::complex<float>> iq;
    if (!read_iq_f32(path, iq)) {
        std::fprintf(stderr, "Failed to read %s\n", path);
        return 2;
    }

    RxConfig cfg; cfg.sf = sf; cfg.bw = bw; cfg.fs = fs; cfg.os = os; cfg.preamble_min = 8;
    Receiver rx(cfg);

    auto res = rx.process(iq);
    if (!res) {
        std::fprintf(stderr, "No frame detected.\n");
        return 3;
    }
    std::printf("Frame start @ raw=%zu (os=%d phase=%d)\n", res->start_sample, res->os, res->phase);
    std::printf("Header bins (%zu): ", res->header_bins.size());
    for (size_t i = 0; i < res->header_bins.size(); ++i) std::printf(i ? ",%u" : "%u", res->header_bins[i]);
    std::printf("\n");
    if (!res->header_bits.empty()) {
        std::printf("Header bits (%zu): ", res->header_bits.size());
        for (size_t i = 0; i < res->header_bits.size(); ++i) std::printf("%u", res->header_bits[i]);
        std::printf("\n");
    }
    if (res->payload_len >= 0) {
        std::printf("Payload length (demo parse): %d\n", res->payload_len);
    }
    if (res->cr_idx > 0) {
        std::printf("Header CR idx: %d (1=4/5,2=4/6,3=4/7,4=4/8)\n", res->cr_idx);
        std::printf("Header has CRC: %s\n", res->has_crc ? "yes" : "no");
        std::printf("Header checksum valid: %s\n", res->header_crc_ok ? "yes" : "no");
    }
    if (!res->payload_bytes.empty()) {
        std::printf("Payload bytes (%zu): ", res->payload_bytes.size());
        for (size_t i = 0; i < res->payload_bytes.size(); ++i) std::printf(i ? " %02X" : "%02X", res->payload_bytes[i]);
        std::printf("\n");
        if (res->has_crc) std::printf("Payload CRC: %s\n", res->payload_crc_ok ? "ok" : "not-ok");
    } else {
        std::printf("Payload bins (%zu): ", res->payload_bins.size());
        for (size_t i = 0; i < res->payload_bins.size(); ++i) std::printf(i ? ",%u" : "%u", res->payload_bins[i]);
        std::printf("\n");
    }
    return 0;
}
