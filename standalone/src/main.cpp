#include "state_machine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace lora::standalone;

enum class SampleFormat { F32, CS16 };

static bool read_iq(const char* path, SampleFormat fmt, std::vector<std::complex<float>>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    if (fmt == SampleFormat::F32) {
        if (sz % (2 * sizeof(float)) != 0) return false;
        size_t count = static_cast<size_t>(sz) / (2 * sizeof(float));
        std::vector<float> buf(count * 2);
        f.read(reinterpret_cast<char*>(buf.data()), buf.size() * sizeof(float));
        if (!f) return false;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) {
            out[i] = {buf[2 * i + 0], buf[2 * i + 1]};
        }
        return true;
    }

    if (sz % (2 * sizeof(int16_t)) != 0) return false;
    size_t count = static_cast<size_t>(sz) / (2 * sizeof(int16_t));
    std::vector<int16_t> buf(count * 2);
    f.read(reinterpret_cast<char*>(buf.data()), buf.size() * sizeof(int16_t));
    if (!f) return false;
    constexpr float scale = 1.0f / 32768.0f;
    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        float i_val = static_cast<float>(buf[2 * i + 0]) * scale;
        float q_val = static_cast<float>(buf[2 * i + 1]) * scale;
        out[i] = {i_val, q_val};
    }
    return true;
}

static void print_usage(const char* prog)
{
    std::fprintf(stderr,
                 "Usage: %s [--json] [--debug-crc] [--dump-bits] [--debug-detect] [--format f32|cs16] [--sync 0x34] <iq_file> [sf=7] [bw=125000] [fs=250000]\n",
                 prog);
}

int main(int argc, char** argv)
{
    bool json_output = false;
    bool debug_crc = false;
    bool dump_bits = false;
    bool debug_detection = false;
    SampleFormat sample_format = SampleFormat::F32;
    unsigned long sync_word = 0x34ul;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json") json_output = true;
        else if (arg == "--debug-crc") debug_crc = true;
        else if (arg == "--dump-bits") dump_bits = true;
        else if (arg == "--debug-detect") debug_detection = true;
        else if (arg == "--format") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for --format\n");
                print_usage(argv[0]);
                return 1;
            }
            std::string v = argv[++i];
            if (v == "f32" || v == "float") {
                sample_format = SampleFormat::F32;
            } else if (v == "cs16" || v == "s16" || v == "iq16") {
                sample_format = SampleFormat::CS16;
            } else {
                std::fprintf(stderr, "Unknown format: %s\n", v.c_str());
                print_usage(argv[0]);
                return 1;
            }
        }
        else if (arg == "--sync") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for --sync\n");
                print_usage(argv[0]);
                return 1;
            }
            std::string v = argv[++i];
            char* end = nullptr;
            unsigned long parsed = std::strtoul(v.c_str(), &end, 0);
            if (end == v.c_str() || parsed > 0xFFul) {
                std::fprintf(stderr, "Invalid sync word: %s\n", v.c_str());
                print_usage(argv[0]);
                return 1;
            }
            sync_word = parsed & 0xFFul;
        }
        else if (arg == "--help" || arg == "-h") { print_usage(argv[0]); return 0; }
        else if (!arg.empty() && arg[0] == '-' && arg != "-") {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string path = positional[0];
    uint32_t sf = (positional.size() > 1) ? static_cast<uint32_t>(std::strtoul(positional[1].c_str(), nullptr, 10)) : 7u;
    uint32_t bw = (positional.size() > 2) ? static_cast<uint32_t>(std::strtoul(positional[2].c_str(), nullptr, 10)) : 125000u;
    uint32_t fs = (positional.size() > 3) ? static_cast<uint32_t>(std::strtoul(positional[3].c_str(), nullptr, 10)) : 250000u;
    uint32_t os = (bw ? fs / bw : 1u);

    std::vector<std::complex<float>> iq;
    if (!read_iq(path.c_str(), sample_format, iq)) {
        std::fprintf(stderr, "Failed to read %s\n", path.c_str());
        return 2;
    }

    RxConfig cfg;
    cfg.sf = sf;
    cfg.bw = bw;
    cfg.fs = fs;
    cfg.os = os;
    cfg.preamble_min = 8;
    cfg.sync_word = static_cast<uint8_t>(sync_word);
    cfg.debug_detection = debug_detection;
    cfg.capture_header_bits = dump_bits;
    cfg.capture_payload_blocks = dump_bits || debug_crc;
    cfg.trace_crc = debug_crc;
    Receiver rx(cfg);

    auto res = rx.process(iq);
    if (!res) {
        std::fprintf(stderr, "No frame detected.\n");
        return 3;
    }
    if (json_output) {
        auto vec_to_string = [](auto const& vec) {
            std::ostringstream oss;
            oss << "[";
            for (size_t idx = 0; idx < vec.size(); ++idx) {
                if (idx) oss << ",";
                oss << static_cast<long long>(vec[idx]);
            }
            oss << "]";
            return oss.str();
        };
        auto vec32_to_string = [](auto const& vec) {
            std::ostringstream oss;
            oss << "[";
            for (size_t idx = 0; idx < vec.size(); ++idx) {
                if (idx) oss << ",";
                oss << vec[idx];
            }
            oss << "]";
            return oss.str();
        };
        std::vector<std::string> fields;
        fields.push_back("\"start_sample\":" + std::to_string(res->start_sample));
        fields.push_back("\"os\":" + std::to_string(res->os));
        fields.push_back("\"phase\":" + std::to_string(res->phase));
        fields.push_back("\"cfo_fraction\":" + std::to_string(res->cfo_fraction));
        fields.push_back("\"cfo_integer\":" + std::to_string(res->cfo_integer));
        fields.push_back("\"sfd_found\":" + std::string(res->sfd_found ? "true" : "false"));
        fields.push_back("\"sfd_decim_pos\":" + std::to_string(res->sfd_decim_pos));
        fields.push_back("\"payload_len\":" + std::to_string(res->payload_len));
        fields.push_back("\"cr_idx\":" + std::to_string(res->cr_idx));
        fields.push_back("\"has_crc\":" + std::string(res->has_crc ? "true" : "false"));
        fields.push_back("\"header_crc_ok\":" + std::string(res->header_crc_ok ? "true" : "false"));
        fields.push_back("\"sync_word\":" + std::to_string(cfg.sync_word));
        fields.push_back("\"payload_crc_ok\":" + std::string(res->payload_crc_ok ? "true" : "false"));
        fields.push_back("\"payload_crc_semtech_ok\":" + std::string(res->payload_crc_semtech_ok ? "true" : "false"));
        fields.push_back("\"payload_crc_gr_ok\":" + std::string(res->payload_crc_gr_ok ? "true" : "false"));
        fields.push_back("\"payload_crc_semtech\":" + std::to_string(res->payload_crc_semtech));
        fields.push_back("\"payload_crc_gr\":" + std::to_string(res->payload_crc_gr));
        fields.push_back("\"payload_crc_rx\":" + std::to_string(res->payload_crc_rx));
        fields.push_back("\"header_bins\":" + vec32_to_string(res->header_bins));
        fields.push_back("\"payload_bins\":" + vec32_to_string(res->payload_bins));
        fields.push_back("\"header_bits\":" + vec_to_string(res->header_bits));
        fields.push_back("\"payload_bytes\":" + vec_to_string(res->payload_bytes));
        fields.push_back("\"payload_bytes_raw\":" + vec_to_string(res->payload_bytes_raw));
        fields.push_back("\"payload_whitening_prns\":" + vec_to_string(res->payload_whitening_prns));
        if (res->header_rows && res->header_cols) {
            fields.push_back("\"header_rows\":" + std::to_string(res->header_rows));
            fields.push_back("\"header_cols\":" + std::to_string(res->header_cols));
            fields.push_back("\"header_inter_bits\":" + vec_to_string(res->header_interleaver_bits));
            fields.push_back("\"header_deinter_bits\":" + vec_to_string(res->header_deinterleaver_bits));
        }
        if (!res->payload_blocks_bits.empty()) {
            std::ostringstream blocks;
            blocks << "[";
            for (size_t bi = 0; bi < res->payload_blocks_bits.size(); ++bi) {
                if (bi) blocks << ",";
                const auto& block = res->payload_blocks_bits[bi];
                blocks << "{\"rows\":" << block.rows
                       << ",\"cols\":" << block.cols
                       << ",\"symbol_offset\":" << static_cast<unsigned long long>(block.symbol_offset)
                       << ",\"inter_bits\":" << vec_to_string(block.inter_bits)
                       << ",\"deinter_bits\":" << vec_to_string(block.deinter_bits)
                       << "}";
            }
            blocks << "]";
            fields.push_back("\"payload_blocks\":" + blocks.str());
        }
        if (!res->payload_crc_trace.empty()) {
            std::ostringstream trace;
            trace << "[";
            for (size_t ti = 0; ti < res->payload_crc_trace.size(); ++ti) {
                if (ti) trace << ",";
                const auto& entry = res->payload_crc_trace[ti];
                trace << "{\"index\":" << entry.index
                      << ",\"raw\":" << static_cast<int>(entry.raw_byte)
                      << ",\"whitening\":" << static_cast<int>(entry.whitening)
                      << ",\"dewhitened\":" << static_cast<int>(entry.dewhitened)
                      << ",\"crc_sem_before\":" << entry.crc_semtech_before
                      << ",\"crc_sem_after\":" << entry.crc_semtech_after
                      << ",\"crc_gr_before\":" << entry.crc_gr_before
                      << ",\"crc_gr_after\":" << entry.crc_gr_after
                      << ",\"count_sem\":" << (entry.counted_semtech ? "true" : "false")
                      << ",\"count_gr\":" << (entry.counted_gr ? "true" : "false")
                      << ",\"is_crc\":" << (entry.is_crc_byte ? "true" : "false")
                      << "}";
            }
            trace << "]";
            fields.push_back("\"payload_crc_trace\":" + trace.str());
        }
        std::ostringstream json;
        json << "{";
        for (size_t idx = 0; idx < fields.size(); ++idx) {
            if (idx) json << ",";
            json << fields[idx];
        }
        json << "}\n";
        std::printf("%s", json.str().c_str());
        return 0;
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
        if (res->has_crc) {
            std::printf("Payload CRC (Semtech spec): %s\n", res->payload_crc_semtech_ok ? "ok" : "not-ok");
            if (debug_crc) {
                std::printf("Payload CRC (gr-lora-sdr variant): %s\n", res->payload_crc_gr_ok ? "ok" : "not-ok");
                std::printf("CRC values: semtech=0x%04X gr=0x%04X rx=0x%04X\n",
                            res->payload_crc_semtech, res->payload_crc_gr, res->payload_crc_rx);
            }
        }
    } else {
        std::printf("Payload bins (%zu): ", res->payload_bins.size());
        for (size_t i = 0; i < res->payload_bins.size(); ++i) std::printf(i ? ",%u" : "%u", res->payload_bins[i]);
        std::printf("\n");
    }
    if (debug_crc && !res->payload_crc_trace.empty()) {
        std::printf("Idx Raw Dewhite PRN CRC_sem_before->after CRC_gr_before->after Flags\n");
        for (const auto& entry : res->payload_crc_trace) {
            std::printf("%3d 0x%02X 0x%02X 0x%02X %04X->%04X %04X->%04X %s%s\n",
                        entry.index,
                        entry.raw_byte,
                        entry.dewhitened,
                        entry.whitening,
                        entry.crc_semtech_before,
                        entry.crc_semtech_after,
                        entry.crc_gr_before,
                        entry.crc_gr_after,
                        entry.counted_semtech ? "S" : "-",
                        entry.counted_gr ? "G" : "-");
        }
    }
    return 0;
}
