#include "lora/workspace.hpp"
#include "lora/rx/loopback_rx.hpp"
#include "lora/rx/frame.hpp"
#include "lora/utils/gray.hpp"
#include "lora/constants.hpp"
#include "lora/debug.hpp"
#include "lora/rx/preamble.hpp"
#include "lora/rx/decimate.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <iostream>

using namespace lora;

enum class Format { AUTO, F32, CS16 };

static void usage(const char* a0) {
    std::fprintf(stderr,
        "Usage: %s --in <iq_file|-> --sf <7..12> --cr <45|46|47|48> [--format auto|f32|cs16] [--min-preamble 8] [--sync auto|0x34|0x12] [--out payload.bin] [--print-header] [--allow-partial] [--json]\n",
        a0);
}

static bool read_f32_iq(const std::string& path, std::vector<std::complex<float>>& out) {
    std::istream* in = nullptr; std::ifstream f;
    if (path == "-") {
        in = &std::cin;
    } else {
        f.open(path, std::ios::binary);
        if (!f) return false; in = &f;
    }
    out.clear(); float buf[2];
    while (in->read(reinterpret_cast<char*>(buf), sizeof(buf))) out.emplace_back(buf[0], buf[1]);
    return !out.empty();
}

static bool read_cs16_iq(const std::string& path, std::vector<std::complex<float>>& out) {
    std::istream* in = nullptr; std::ifstream f;
    if (path == "-") {
        in = &std::cin;
    } else {
        f.open(path, std::ios::binary);
        if (!f) return false; in = &f;
    }
    out.clear(); int16_t buf[2]; const float s = 1.0f/32768.0f;
    while (in->read(reinterpret_cast<char*>(buf), sizeof(buf))) out.emplace_back(buf[0]*s, buf[1]*s);
    return !out.empty();
}

int main(int argc, char** argv) {
    std::string in_path; int sf = 0; int cr_int = 0; Format fmt = Format::AUTO;
    int min_pre = 8; unsigned int sync_hex = lora::LORA_SYNC_WORD_PUBLIC; bool sync_auto = false; std::string out_path;
    bool user_min_pre = false;
    bool print_header = false; bool allow_partial = false; bool json = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--in" && i+1 < argc) in_path = argv[++i];
        else if (a == "--sf" && i+1 < argc) sf = std::stoi(argv[++i]);
        else if (a == "--cr" && i+1 < argc) cr_int = std::stoi(argv[++i]);
        else if (a == "--format" && i+1 < argc) {
            std::string v = argv[++i];
            if (v == "auto") fmt = Format::AUTO; else if (v == "f32") fmt = Format::F32; else if (v == "cs16") fmt = Format::CS16; else { usage(argv[0]); return 2; }
        }
        else if (a == "--min-preamble" && i+1 < argc) { min_pre = std::stoi(argv[++i]); user_min_pre = true; }
        else if (a == "--sync" && i+1 < argc) {
            std::string v = argv[++i];
            if (v == "auto" || v == "AUTO") { sync_auto = true; sync_hex = lora::LORA_SYNC_WORD_PUBLIC; }
            else sync_hex = std::stoul(v, nullptr, 0);
        }
        else if (a == "--out" && i+1 < argc) out_path = argv[++i];
        else if (a == "--print-header") print_header = true;
        else if (a == "--allow-partial") allow_partial = true;
        else if (a == "--json") json = true;
        else { usage(argv[0]); return 2; }
    }
    if (in_path.empty() || sf < 7 || sf > 12 || cr_int < 45 || cr_int > 48) { usage(argv[0]); return 2; }

    // Deduce format if AUTO
    if (fmt == Format::AUTO && in_path != "-") {
        try {
            auto sz = std::filesystem::file_size(in_path);
            // Heuristic: prefer f32 if divisible by 8; else cs16
            if (sz % 8 == 0) fmt = Format::F32; else fmt = Format::CS16;
        } catch (...) { fmt = Format::F32; }
    }
    if (fmt == Format::AUTO) fmt = Format::F32; // stdin default

    // Auto-calibrate minimal preamble symbols by SF if user did not specify
    if (!user_min_pre) {
        if (sf >= 10)      min_pre = 12;
        else if (sf == 9)  min_pre = 10;
        else               min_pre = 8;
    }

    std::vector<std::complex<float>> iq;
    bool ok = (fmt == Format::F32) ? read_f32_iq(in_path, iq) : read_cs16_iq(in_path, iq);
    if (!ok) { std::fprintf(stderr, "Failed to read IQ from %s\n", in_path.c_str()); return 3; }

    lora::utils::CodeRate cr = lora::utils::CodeRate::CR45;
    switch (cr_int) { case 45: cr = lora::utils::CodeRate::CR45; break; case 46: cr = lora::utils::CodeRate::CR46; break; case 47: cr = lora::utils::CodeRate::CR47; break; case 48: cr = lora::utils::CodeRate::CR48; break; }

    Workspace ws;
    auto span = std::span<const std::complex<float>>(iq.data(), iq.size());
    // Pre-detect OS/phase/start for diagnostics and JSON
    int det_os = -1, det_phase = -1; size_t det_start = 0;
    {
        auto det = lora::rx::detect_preamble_os(ws, span, (uint32_t)sf, (size_t)min_pre, {1,2,4,8});
        if (det) { det_os = det->os; det_phase = det->phase; det_start = det->start_sample; }
        if (std::getenv("LORA_DEBUG")) {
            if (det) std::fprintf(stderr, "[lora_decode] det os=%d phase=%d start=%zu\n", det->os, det->phase, det->start_sample);
            else std::fprintf(stderr, "[lora_decode] preamble not detected\n");
        }
    }
    auto res = lora::rx::loopback_rx_header_auto_sync(ws, span, static_cast<uint32_t>(sf), cr, static_cast<size_t>(min_pre), true, static_cast<uint8_t>(sync_hex & 0xFF));
    if (!res.second) {
        auto map_exit = [](int step)->std::pair<const char*, int>{
            switch (step) {
                case 100: case 102: return {"preamble_not_found", 3};
                case 103: case 104: return {"sync_estimation_failed", 8};
                case 107: return {"sync_mismatch", 4};
                case 109: return {"header_crc_failed", 5};
                case 111: return {"fec_decode_failed", 7};
                case 112: return {"payload_crc_failed", 6};
                case 101: case 105: case 106: case 108: case 110: return {"insufficient_or_misaligned_iq", 9};
                default: return {"decode_failed", 4};
            }
        };
        // Fallbacks: try alternative sync, and vary preamble length heuristically
        uint8_t sync_alt = (static_cast<uint8_t>(sync_hex & 0xFF) == lora::LORA_SYNC_WORD_PUBLIC) ? lora::LORA_SYNC_WORD_PRIVATE : lora::LORA_SYNC_WORD_PUBLIC;
        if (sync_auto) { sync_hex = lora::LORA_SYNC_WORD_PUBLIC; sync_alt = lora::LORA_SYNC_WORD_PRIVATE; }
        int pre_alts[4] = {min_pre, 6, 10, 12};
        bool ok_any = false;
        for (int pre : pre_alts) {
            auto r2 = lora::rx::loopback_rx_header_auto_sync(ws, span, static_cast<uint32_t>(sf), cr, static_cast<size_t>(pre), true, static_cast<uint8_t>(sync_hex & 0xFF));
            if (r2.second) { res = r2; ok_any = true; break; }
            auto r3 = lora::rx::loopback_rx_header_auto_sync(ws, span, static_cast<uint32_t>(sf), cr, static_cast<size_t>(pre), true, sync_alt);
            if (r3.second) { res = r3; ok_any = true; break; }
        }
        if (!ok_any) {
            // Try to decode header and optionally payload without CRC for diagnostics
            auto hdr = lora::rx::decode_header_with_preamble_cfo_sto_os(ws, span, static_cast<uint32_t>(sf), cr, static_cast<size_t>(min_pre), static_cast<uint8_t>(sync_hex & 0xFF));
            if (hdr.has_value()) {
                std::fprintf(stderr, "[header] len=%u cr=%d crc=%s\n", hdr->payload_len, int(hdr->cr), hdr->has_crc?"true":"false");
                if (allow_partial) {
                    auto pay = lora::rx::decode_payload_no_crc_with_preamble_cfo_sto_os(ws, span, static_cast<uint32_t>(sf), cr, static_cast<size_t>(min_pre), static_cast<uint8_t>(sync_hex & 0xFF));
                    auto &payload = pay.first;
                    if (!payload.empty()) {
                        std::fprintf(stderr, "[warn] CRC check failed earlier; returning payload without CRC verification.\n");
                        if (json) {
                            std::string hex; hex.reserve(payload.size()*2);
                            static const char* H = "0123456789abcdef";
                            for (auto b : payload) { hex.push_back(H[(b>>4)&0xF]); hex.push_back(H[b&0xF]); }
                            if (!out_path.empty()) {
                                std::ofstream of(out_path, std::ios::binary);
                                if (of) of.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
                            }
                            std::fprintf(stdout,
                                "{\n  \"success\": false,\n  \"step\": %d,\n  \"reason\": \"payload_crc_failed\",\n  \"sf\": %d,\n  \"cr\": %d,\n  \"sync\": \"0x%02x\",\n  \"min_preamble\": %d,\n  \"detect_os\": %d,\n  \"detect_phase\": %d,\n  \"detect_start\": %zu,\n  \"header\": {\"len\": %u, \"cr\": %d, \"crc\": %s},\n  \"payload_len\": %zu,\n  \"payload_hex\": \"%s\"%s\n}\n",
                                lora::debug::last_fail_step, sf, cr_int, (unsigned)(sync_hex & 0xFF), min_pre,
                                det_os, det_phase, det_start,
                                (unsigned)hdr->payload_len, int(hdr->cr), (hdr->has_crc?"true":"false"),
                                payload.size(), hex.c_str(), out_path.empty()?"":",\n  \"payload_file\": \"written\""
                            );
                            return 6;
                        } else {
                            if (!out_path.empty()) {
                                std::ofstream of(out_path, std::ios::binary);
                                if (!of) { std::fprintf(stderr, "Failed to open out file %s\n", out_path.c_str()); return 5; }
                                of.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
                                return 0;
                            }
                            for (size_t i = 0; i < payload.size(); ++i) {
                                std::printf("%02x", payload[i]); if (i+1 < payload.size()) std::printf(" ");
                            }
                            std::printf("\n");
                            return 0;
                        }
                    }
                }
            }
            auto [reason, code] = map_exit(lora::debug::last_fail_step);
            if (json) {
                // If we already produced payload diagnostics (A3/A4) due to a diagnostic fallback, emit them first
                if (std::string(reason) == std::string("payload_crc_failed") && !ws.dbg_postdew.empty()) {
                    auto to_hex_str = [](const std::vector<uint8_t>& v){
                        static const char* H = "0123456789abcdef";
                        std::string s; size_t lim = std::min<size_t>(v.size(), 32);
                        s.reserve(lim*3);
                        for (size_t i = 0; i < lim; ++i) { s.push_back(H[(v[i]>>4)&0xF]); s.push_back(H[v[i]&0xF]); if (i+1<lim) s.push_back(' '); }
                        return s;
                    };
                    std::filesystem::create_directories("logs");
                    if (!ws.dbg_predew.empty()) { std::ofstream f("logs/lite_rx_predew.bin", std::ios::binary); if (f) f.write((const char*)ws.dbg_predew.data(), (std::streamsize)ws.dbg_predew.size()); }
                    if (!ws.dbg_postdew.empty()) { std::ofstream f("logs/lite_rx_postdew.bin", std::ios::binary); if (f) f.write((const char*)ws.dbg_postdew.data(), (std::streamsize)ws.dbg_postdew.size()); }
                    int cr_denom = 4 + (int)ws.dbg_cr_payload;
                    std::fprintf(stdout,
                        "{\n  \"hdr\": {\"sf\":%d,\"cr_hdr\":\"4/8\",\"cr_payload\":\"4/%d\",\"payload_len\":%u},\n  \"predew_hex\": \"%s\",\n  \"postdew_hex\": \"%s\",\n  \"crc_calc\": \"%04x\",\n  \"crc_rx_le\": \"%04x\",\n  \"crc_rx_be\": \"%04x\",\n  \"crc_ok_le\": %s, \"crc_ok_be\": %s\n}\n",
                        sf, cr_denom, (unsigned)ws.dbg_payload_len,
                        to_hex_str(ws.dbg_predew).c_str(), to_hex_str(ws.dbg_postdew).c_str(),
                        (unsigned)ws.dbg_crc_calc, (unsigned)ws.dbg_crc_rx_le, (unsigned)ws.dbg_crc_rx_be,
                        ws.dbg_crc_ok_le?"true":"false", ws.dbg_crc_ok_be?"true":"false"
                    );
                    return code;
                }

                auto hdr2 = lora::rx::decode_header_with_preamble_cfo_sto_os(ws, span, static_cast<uint32_t>(sf), cr, static_cast<size_t>(min_pre), static_cast<uint8_t>(sync_hex & 0xFF));
                if (hdr2.has_value()) {
                    // If payload CRC failed, emit detailed instrumentation JSON and write dumps
                    if (std::string(reason) == std::string("payload_crc_failed")) {
                        auto to_hex_str = [](const std::vector<uint8_t>& v){
                            static const char* H = "0123456789abcdef";
                            std::string s; size_t lim = std::min<size_t>(v.size(), 32);
                            s.reserve(lim*3);
                            for (size_t i = 0; i < lim; ++i) { s.push_back(H[(v[i]>>4)&0xF]); s.push_back(H[v[i]&0xF]); if (i+1<lim) s.push_back(' '); }
                            return s;
                        };
                        std::filesystem::create_directories("logs");
                        if (!ws.dbg_predew.empty()) { std::ofstream f("logs/lite_rx_predew.bin", std::ios::binary); if (f) f.write((const char*)ws.dbg_predew.data(), (std::streamsize)ws.dbg_predew.size()); }
                        if (!ws.dbg_postdew.empty()) { std::ofstream f("logs/lite_rx_postdew.bin", std::ios::binary); if (f) f.write((const char*)ws.dbg_postdew.data(), (std::streamsize)ws.dbg_postdew.size()); }
                        int cr_denom = 4 + (int)ws.dbg_cr_payload;
                        std::fprintf(stdout,
                            "{\n  \"hdr\": {\"sf\":%d,\"cr_hdr\":\"4/8\",\"cr_payload\":\"4/%d\",\"payload_len\":%u},\n  \"predew_hex\": \"%s\",\n  \"postdew_hex\": \"%s\",\n  \"crc_calc\": \"%04x\",\n  \"crc_rx_le\": \"%04x\",\n  \"crc_rx_be\": \"%04x\",\n  \"crc_ok_le\": %s, \"crc_ok_be\": %s\n}\n",
                            sf, cr_denom, (unsigned)ws.dbg_payload_len,
                            to_hex_str(ws.dbg_predew).c_str(), to_hex_str(ws.dbg_postdew).c_str(),
                            (unsigned)ws.dbg_crc_calc, (unsigned)ws.dbg_crc_rx_le, (unsigned)ws.dbg_crc_rx_be,
                            ws.dbg_crc_ok_le?"true":"false", ws.dbg_crc_ok_be?"true":"false"
                        );
                    } else {
                        // Header parsed but failure elsewhere; emit header info
                        std::fprintf(stdout,
                            "{\n  \"success\": false,\n  \"step\": %d,\n  \"reason\": \"%s\",\n  \"sf\": %d,\n  \"cr\": %d,\n  \"sync\": \"0x%02x\",\n  \"min_preamble\": %d,\n  \"detect_os\": %d,\n  \"detect_phase\": %d,\n  \"detect_start\": %zu,\n  \"header\": {\"len\": %u, \"cr\": %d, \"crc\": %s},\n  \"payload_len\": 0,\n  \"payload_hex\": \"\"\n}\n",
                            lora::debug::last_fail_step, reason, sf, cr_int, (unsigned)(sync_hex & 0xFF), min_pre,
                            det_os, det_phase, det_start,
                            (unsigned)hdr2->payload_len, int(hdr2->cr), (hdr2->has_crc?"true":"false")
                        );
                    }
                } else {
                    // Header decode failed: print header diagnostics if available
                    if (ws.dbg_hdr_filled) {
                        auto append_u32 = [](std::string& s, uint32_t v){ char buf[16]; std::snprintf(buf, sizeof(buf), "%u", v); if (!s.empty()) s += ","; s += buf; };
                        auto append_hex = [](std::string& s, uint8_t v){ char buf[8]; std::snprintf(buf, sizeof(buf), "%x", v & 0xF); if (!s.empty()) s += " "; s += buf; };
                        std::string syms_raw, syms_corr, syms_gray, n48, n45;
                        for (int i = 0; i < 16; ++i) { append_u32(syms_raw, ws.dbg_hdr_syms_raw[i]); append_u32(syms_corr, ws.dbg_hdr_syms_corr[i]); append_u32(syms_gray, ws.dbg_hdr_gray[i]); }
                        for (int i = 0; i < 10; ++i) { append_hex(n48, ws.dbg_hdr_nibbles_cr48[i]); append_hex(n45, ws.dbg_hdr_nibbles_cr45[i]); }
                        std::fprintf(stdout,
                            "{\n  \"success\": false,\n  \"step\": %d,\n  \"reason\": \"%s\",\n  \"sf\": %d,\n  \"cr\": %d,\n  \"sync\": \"0x%02x\",\n  \"min_preamble\": %d,\n  \"detect_os\": %d,\n  \"detect_phase\": %d,\n  \"detect_start\": %zu,\n  \"header\": null,\n  \"dbg_hdr\": {\"sf\": %u, \"syms_raw\": \"%s\", \"syms_corr\": \"%s\", \"syms_gray\": \"%s\", \"nibbles_cr48\": \"%s\", \"nibbles_cr45\": \"%s\"}\n}\n",
                            lora::debug::last_fail_step, reason, sf, cr_int, (unsigned)(sync_hex & 0xFF), min_pre,
                            det_os, det_phase, det_start,
                            (unsigned)ws.dbg_hdr_sf, syms_raw.c_str(), syms_corr.c_str(), syms_gray.c_str(), n48.c_str(), n45.c_str());
                    } else {
                        std::fprintf(stdout,
                            "{\n  \"success\": false,\n  \"step\": %d,\n  \"reason\": \"%s\",\n  \"sf\": %d,\n  \"cr\": %d,\n  \"sync\": \"0x%02x\",\n  \"min_preamble\": %d,\n  \"detect_os\": %d,\n  \"detect_phase\": %d,\n  \"detect_start\": %zu,\n  \"header\": null,\n  \"payload_len\": 0,\n  \"payload_hex\": \"\"\n}\n",
                            lora::debug::last_fail_step, reason, sf, cr_int, (unsigned)(sync_hex & 0xFF), min_pre,
                            det_os, det_phase, det_start
                        );
                    }
                }
            } else {
                std::fprintf(stderr,
                    "Decode failed (step=%d, reason=%s). detect_os=%d detect_phase=%d detect_start=%zu. Hints: try --sync auto/0x12 or adjust --min-preamble.\n",
                    lora::debug::last_fail_step, reason, det_os, det_phase, det_start);
            }
            return code;
        }
    }
    auto payload = res.first;
    if (json) {
        auto hdr = lora::rx::decode_header_with_preamble_cfo_sto_os(ws, span, (uint32_t)sf, cr, (size_t)min_pre, (uint8_t)(sync_hex & 0xFF));
        std::string hex; hex.reserve(payload.size()*2);
        static const char* H = "0123456789abcdef";
        for (auto b : payload) { hex.push_back(H[(b>>4)&0xF]); hex.push_back(H[b&0xF]); }
        if (!out_path.empty()) { std::ofstream of(out_path, std::ios::binary); if (of) of.write(reinterpret_cast<const char*>(payload.data()), (std::streamsize)payload.size()); }
        if (hdr.has_value()) {
            std::fprintf(stdout,
                "{\n  \"success\": true,\n  \"step\": 0,\n  \"reason\": null,\n  \"sf\": %d,\n  \"cr\": %d,\n  \"sync\": \"0x%02x\",\n  \"min_preamble\": %d,\n  \"detect_os\": %d,\n  \"detect_phase\": %d,\n  \"detect_start\": %zu,\n  \"header\": {\"len\": %u, \"cr\": %d, \"crc\": %s},\n  \"payload_len\": %zu,\n  \"payload_hex\": \"%s\"%s\n}\n",
                sf, cr_int, (unsigned)(sync_hex & 0xFF), min_pre, det_os, det_phase, det_start,
                (unsigned)hdr->payload_len, int(hdr->cr), (hdr->has_crc?"true":"false"),
                payload.size(), hex.c_str(), out_path.empty()?"":",\n  \"payload_file\": \"written\""
            );
        } else {
            std::fprintf(stdout,
                "{\n  \"success\": true,\n  \"step\": 0,\n  \"reason\": null,\n  \"sf\": %d,\n  \"cr\": %d,\n  \"sync\": \"0x%02x\",\n  \"min_preamble\": %d,\n  \"detect_os\": %d,\n  \"detect_phase\": %d,\n  \"detect_start\": %zu,\n  \"header\": null,\n  \"payload_len\": %zu,\n  \"payload_hex\": \"%s\"%s\n}\n",
                sf, cr_int, (unsigned)(sync_hex & 0xFF), min_pre, det_os, det_phase, det_start,
                payload.size(), hex.c_str(), out_path.empty()?"":",\n  \"payload_file\": \"written\""
            );
        }
        return 0;
    }
    if (print_header) {
        auto hdr = lora::rx::decode_header_with_preamble_cfo_sto_os(ws, span, (uint32_t)sf, cr, (size_t)min_pre, (uint8_t)(sync_hex & 0xFF));
        if (hdr.has_value())
            std::fprintf(stderr, "[header] len=%u cr=%d crc=%s\n", (unsigned)hdr->payload_len, int(hdr->cr), hdr->has_crc?"true":"false");
    }
    if (!out_path.empty()) {
        std::ofstream of(out_path, std::ios::binary);
        if (!of) { std::fprintf(stderr, "Failed to open out file %s\n", out_path.c_str()); return 5; }
        of.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        return 0;
    }
    for (size_t i = 0; i < payload.size(); ++i) {
        std::printf("%02x", payload[i]); if (i+1 < payload.size()) std::printf(" " );
    }
    std::printf("\n");
    return 0;
}
