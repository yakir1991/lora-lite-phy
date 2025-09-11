#include "lora/workspace.hpp"
#include "lora/rx/loopback_rx.hpp"
#include "lora/rx/frame.hpp"
#include "lora/utils/gray.hpp"
#include "lora/constants.hpp"
#include "lora/utils/whitening.hpp"
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

// Local demod helper (same as rx/frame.cpp demod_symbol)
static uint32_t demod_symbol_local(Workspace& ws, const std::complex<float>* block) {
    uint32_t N = ws.N;
    for (uint32_t n = 0; n < N; ++n)
        ws.rxbuf[n] = block[n] * ws.downchirp[n];
    ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
    uint32_t max_bin = 0; float max_mag = 0.f;
    for (uint32_t k = 0; k < N; ++k) {
        float mag = std::norm(ws.fftbuf[k]);
        if (mag > max_mag) { max_mag = mag; max_bin = k; }
    }
    return max_bin;
}

int main(int argc, char** argv) {
    std::string in_path; int sf = 0; int cr_int = 0; Format fmt = Format::AUTO;
    int min_pre = 8; unsigned int sync_hex = lora::LORA_SYNC_WORD_PUBLIC; bool sync_auto = false; std::string out_path;
    bool user_min_pre = false;
    bool print_header = false; bool allow_partial = false; bool json = false;
    int force_hdr_len = -1; int force_hdr_cr = 0; int force_hdr_crc = -1;
    bool hdr_scan = false;

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
        else if (a == "--hdr-scan") hdr_scan = true;
        else if (a == "--force-hdr-len" && i+1 < argc) force_hdr_len = std::stoi(argv[++i]);
        else if (a == "--force-hdr-cr" && i+1 < argc) force_hdr_cr = std::stoi(argv[++i]);
        else if (a == "--force-hdr-crc" && i+1 < argc) force_hdr_crc = std::stoi(argv[++i]);
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

    if (hdr_scan) {
        // Perform a small header window scan and dump multiple syms_raw snapshots for offline analysis
        lora::Workspace ws;
        ws.init((uint32_t)sf);
        // Detect OS and phase
        auto det = lora::rx::detect_preamble_os(ws, std::span<const std::complex<float>>(iq.data(), iq.size()), (uint32_t)sf, (size_t)min_pre, {4,2,1,8});
        if (!det) { std::fprintf(stderr, "[hdr-scan] preamble_os detect failed\n"); return 4; }
        // Decimate to OS=1
        auto decim = lora::rx::decimate_os_phase(std::span<const std::complex<float>>(iq.data(), iq.size()), det->os, det->phase);
        size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
        if (start_decim >= decim.size()) { std::fprintf(stderr, "[hdr-scan] start_decim OOB\n"); return 4; }
        auto aligned0 = std::span<const std::complex<float>>(decim.data() + start_decim, decim.size() - start_decim);
        // Detect preamble on OS=1 span
        auto pos0 = lora::rx::detect_preamble(ws, aligned0, (uint32_t)sf, (size_t)min_pre);
        if (!pos0) { std::fprintf(stderr, "[hdr-scan] preamble detect (OS=1) failed\n"); return 4; }
        // Estimate CFO and compensate
        auto cfo = lora::rx::estimate_cfo_from_preamble(ws, aligned0, (uint32_t)sf, *pos0, (size_t)min_pre);
        float cfo_val = cfo.has_value() ? *cfo : 0.0f;
        std::vector<std::complex<float>> comp(aligned0.size());
        {
            float two_pi_eps = -2.0f * static_cast<float>(M_PI) * cfo_val;
            std::complex<float> j(0.f, 1.f);
            for (size_t n = 0; n < aligned0.size(); ++n)
                comp[n] = aligned0[n] * std::exp(j * (two_pi_eps * static_cast<float>(n)));
        }
        // Estimate integer STO (optional)
        auto sto = lora::rx::estimate_sto_from_preamble(ws, comp, (uint32_t)sf, *pos0, (size_t)min_pre, static_cast<int>(ws.N/8));
        int shift = sto.has_value() ? *sto : 0;
        size_t aligned_start = (shift >= 0) ? (*pos0 + static_cast<size_t>(shift)) : (*pos0 - static_cast<size_t>(-shift));
        if (aligned_start >= comp.size()) { std::fprintf(stderr, "[hdr-scan] aligned_start OOB\n"); return 4; }
        auto aligned = std::span<const std::complex<float>>(comp.data() + aligned_start, comp.size() - aligned_start);
        // Header nominal start: sync + 2 downchirps + 0.25 symbol (2.25 symbols after min_pre)
        ws.init((uint32_t)sf);
        uint32_t N = ws.N;
        size_t hdr_start_base = (size_t)min_pre * N + (2u * N + N/4u);
        if (hdr_start_base + N > aligned.size()) { std::fprintf(stderr, "[hdr-scan] not enough samples for header base\n"); return 4; }
        // Scan small sets
        std::vector<int> samp_shifts = {0, (int)N/64, -(int)N/64, (int)N/32, -(int)N/32};
        std::vector<int> off0_list = {0,1,2};
        std::vector<int> off1_list = {0,1,2,3,4,5,6,7};
        // Open output JSON
        FILE* f = std::fopen("logs/lite_hdr_scan.json", "w");
        if (!f) { std::fprintf(stderr, "[hdr-scan] failed to open logs/lite_hdr_scan.json\n"); return 5; }
        std::fprintf(f, "[\n"); bool first = true;
        for (int off0d : off0_list) {
            for (int ss0 : samp_shifts) {
                // index for block0
                size_t idx0;
                if (ss0 >= 0) { idx0 = hdr_start_base + (size_t)off0d * N + (size_t)ss0; }
                else { size_t o = (size_t)(-ss0); if (hdr_start_base + (size_t)off0d * N < o) continue; idx0 = hdr_start_base + (size_t)off0d * N - o; }
                if (idx0 + 8u * N > aligned.size()) continue;
                // Demod 8 symbols for block0
                uint32_t raw0[8]{};
                for (size_t s = 0; s < 8; ++s) raw0[s] = demod_symbol_local(ws, &aligned[idx0 + s * N]);
                for (int off1d : off1_list) {
                    for (int ss1 : samp_shifts) {
                        size_t idx1_base = hdr_start_base + 8u * N + (size_t)off1d * N;
                        size_t idx1;
                        if (ss1 >= 0) { idx1 = idx1_base + (size_t)ss1; }
                        else { size_t o = (size_t)(-ss1); if (idx1_base < o) continue; idx1 = idx1_base - o; }
                        if (idx1 + 8u * N > aligned.size()) continue;
                        uint32_t raw1[8]{}; for (size_t s = 0; s < 8; ++s) raw1[s] = demod_symbol_local(ws, &aligned[idx1 + s * N]);
                        // Emit JSON entry
                        if (!first) std::fprintf(f, ",\n"); first = false;
                        std::fprintf(f, "  {\"off0\":%d,\"samp0\":%d,\"off1\":%d,\"samp1\":%d,\"syms_raw\":[",
                                     off0d, ss0, off1d, ss1);
                        for (int i = 0; i < 16; ++i) {
                            uint32_t v = (i < 8) ? raw0[i] : raw1[i-8];
                            std::fprintf(f, "%u%s", (unsigned)v, (i+1<16)?",":"");
                        }
                        std::fprintf(f, "]}");
                    }
                }
            }
        }
        std::fprintf(f, "\n]\n");
        std::fclose(f);
        std::fprintf(stderr, "[hdr-scan] wrote logs/lite_hdr_scan.json (%d off0 * %zu samp0 * %zu off1 * %zu samp1 combinations)\n",
                      (int)off0_list.size(), samp_shifts.size(), off1_list.size(), samp_shifts.size());
        return 0;
    }

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
    // If user forces header parameters, bypass header parsing entirely
    if (force_hdr_len >= 0 && force_hdr_crc != -1) {
        lora::rx::LocalHeader forced; forced.payload_len = static_cast<uint8_t>(force_hdr_len);
        forced.has_crc = (force_hdr_crc != 0);
        switch (force_hdr_cr) {
            case 1: forced.cr = lora::utils::CodeRate::CR45; break;
            case 2: forced.cr = lora::utils::CodeRate::CR46; break;
            case 3: forced.cr = lora::utils::CodeRate::CR47; break;
            case 4: forced.cr = lora::utils::CodeRate::CR48; break;
            default: forced.cr = cr; break;
        }
        // Align (OS-aware) → CFO → STO like normal path
        auto det = lora::rx::detect_preamble_os(ws, span, static_cast<uint32_t>(sf), (size_t)min_pre, {4,2,1,8});
        if (!det) { std::fprintf(stderr, "[force] preamble detect failed\n"); return 3; }
        auto decim = lora::rx::decimate_os_phase(span, det->os, det->phase);
        size_t start_decim = det->start_sample / (size_t)det->os;
        if (start_decim >= decim.size()) return 3;
        auto aligned0 = std::span<const std::complex<float>>(decim.data() + start_decim, decim.size() - start_decim);
        ws.init((uint32_t)sf);
        auto pos0 = lora::rx::detect_preamble(ws, aligned0, (uint32_t)sf, (size_t)min_pre);
        if (!pos0) return 3;
        auto cfo_opt = lora::rx::estimate_cfo_from_preamble(ws, aligned0, (uint32_t)sf, *pos0, (size_t)min_pre);
        float cfo_val = cfo_opt.has_value() ? *cfo_opt : 0.f;
        std::vector<std::complex<float>> comp(aligned0.size());
        {
            float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (cfo_val);
            std::complex<float> j(0.f, 1.f);
            for (size_t n = 0; n < aligned0.size(); ++n)
                comp[n] = aligned0[n] * std::exp(j * (two_pi_eps * static_cast<float>(n)));
        }
        auto sto_opt = lora::rx::estimate_sto_from_preamble(ws, comp, (uint32_t)sf, *pos0, (size_t)min_pre, (int)(ws.N/8));
        int shift = sto_opt.has_value() ? *sto_opt : 0;
        size_t aligned_start = (shift >= 0) ? (*pos0 + (size_t)shift) : (*pos0 - (size_t)(-shift));
        if (aligned_start >= comp.size()) return 3;
        auto aligned = std::span<const std::complex<float>>(comp.data() + aligned_start, comp.size() - aligned_start);
        // Header anchor at sync + 2.25 symbols
        size_t header_start = (size_t)min_pre * ws.N + (2u * ws.N + ws.N/4u);
        if (header_start >= aligned.size()) return 3;
        auto data = std::span<const std::complex<float>>(aligned.data() + header_start, aligned.size() - header_start);
        // Compute header nsym (padded): hdr_bits_exact=80, block_bits=sf*8
        const uint32_t header_cr_plus4 = 8u;
        size_t hdr_bits_exact = 5u * 2u * header_cr_plus4;
        uint32_t header_block_bits = (uint32_t)sf * header_cr_plus4;
        size_t hdr_bits_padded = hdr_bits_exact;
        if (hdr_bits_padded % header_block_bits) hdr_bits_padded = ((hdr_bits_padded / header_block_bits) + 1) * header_block_bits;
        size_t hdr_nsym = hdr_bits_padded / (size_t)sf; // typically 16 at SF7
        // Decode payload using forced CR directly after header nsym
        uint32_t payload_cr_plus4 = (uint32_t)forced.cr + 4u;
        size_t pay_crc_bytes = (size_t)forced.payload_len + 2u;
        size_t pay_bits_exact = pay_crc_bytes * 2u * payload_cr_plus4;
        uint32_t payload_block_bits = (uint32_t)sf * payload_cr_plus4;
        size_t pay_bits_padded = pay_bits_exact;
        if (pay_bits_padded % payload_block_bits) pay_bits_padded = ((pay_bits_padded / payload_block_bits) + 1) * payload_block_bits;
        size_t pay_nsym = pay_bits_padded / (size_t)sf;
        if (data.size() < (hdr_nsym + pay_nsym) * ws.N) {
            // insufficient samples
            if (json) {
                std::fprintf(stdout,
                    "{\n  \"success\": false,\n  \"step\": 0,\n  \"reason\": \"header_forced\",\n  \"sf\": %d,\n  \"cr\": %d,\n  \"sync\": \"0x%02x\",\n  \"min_preamble\": %d,\n  \"detect_os\": %d,\n  \"detect_phase\": %d,\n  \"detect_start\": %zu,\n  \"header\": {\"len\": %u, \"cr\": %d, \"crc\": %s},\n  \"payload_len\": 0,\n  \"payload_hex\": \"\"\n}\n",
                    sf, cr_int, (unsigned)(sync_hex & 0xFF), min_pre, det_os, det_phase, det_start,
                    (unsigned)forced.payload_len, int(forced.cr), (forced.has_crc?"true":"false"));
            }
            return 0;
        }
        // Demodulate payload symbols
        ws.ensure_rx_buffers(pay_nsym, (uint32_t)sf, payload_cr_plus4);
        auto& symbols_pay = ws.rx_symbols; symbols_pay.resize(pay_nsym);
        for (size_t s = 0; s < pay_nsym; ++s) {
            uint32_t raw_symbol = 0;
            // reuse demod from rx/frame.cpp
            // inline demod: multiply by downchirp and FFT peak is hidden; use helper in this TU? We don't have it; approximate via reuse from rx path is not trivial here.
            // Fallback: call lora::rx::decode_payload_no_crc_with_preamble_cfo_sto_os which demods for us (it aligns internally), but we already aligned. Keep earlier simple path for now.
        }
        // For simplicity, fallback to existing helper for payload demod (alignment handled inside)
        auto pay = lora::rx::decode_payload_no_crc_with_preamble_cfo_sto_os(ws, span, static_cast<uint32_t>(sf), forced.cr, static_cast<size_t>(min_pre), static_cast<uint8_t>(sync_hex & 0xFF));
        if (json) {
            std::string hex; hex.reserve(pay.first.size()*2);
            static const char* H = "0123456789abcdef";
            for (auto b : pay.first) { hex.push_back(H[(b>>4)&0xF]); hex.push_back(H[b&0xF]); }
            std::fprintf(stdout,
                "{\n  \"success\": false,\n  \"step\": 0,\n  \"reason\": \"header_forced\",\n  \"sf\": %d,\n  \"cr\": %d,\n  \"sync\": \"0x%02x\",\n  \"min_preamble\": %d,\n  \"detect_os\": %d,\n  \"detect_phase\": %d,\n  \"detect_start\": %zu,\n  \"header\": {\"len\": %u, \"cr\": %d, \"crc\": %s},\n  \"payload_len\": %zu,\n  \"payload_hex\": \"%s\"\n}\n",
                sf, cr_int, (unsigned)(sync_hex & 0xFF), min_pre,
                det_os, det_phase, det_start,
                (unsigned)forced.payload_len, int(forced.cr), (forced.has_crc?"true":"false"),
                pay.first.size(), hex.c_str());
            return 0;
        }
        // Non-JSON path: print raw payload if requested elsewhere, otherwise exit
        return pay.first.empty() ? 6 : 0;
    }

    auto res = lora::rx::loopback_rx_header_auto_sync(ws, span, static_cast<uint32_t>(sf), cr, static_cast<size_t>(min_pre), true, static_cast<uint8_t>(sync_hex & 0xFF));
    if (!res.second) {
        // Forced header override (diagnostics): skip parsing and use provided header parameters
        if (force_hdr_len >= 0 && force_hdr_crc != -1) {
            lora::rx::LocalHeader forced; forced.payload_len = static_cast<uint8_t>(force_hdr_len);
            forced.has_crc = (force_hdr_crc != 0);
            switch (force_hdr_cr) {
                case 1: forced.cr = lora::utils::CodeRate::CR45; break;
                case 2: forced.cr = lora::utils::CodeRate::CR46; break;
                case 3: forced.cr = lora::utils::CodeRate::CR47; break;
                case 4: forced.cr = lora::utils::CodeRate::CR48; break;
                default: forced.cr = cr;
            }
            // Attempt payload decode using forced header (direct demod from aligned samples)
            {
                // Header anchor and symbol counts (note: here aligned may not be available in this scope; use span directly)
                auto aligned = std::span<const std::complex<float>>(span.data(), span.size());
                size_t header_start = (size_t)min_pre * ws.N + (2u * ws.N + ws.N/4u);
                auto data = std::span<const std::complex<float>>(aligned.data() + header_start, aligned.size() - header_start);
                const uint32_t payload_cr_plus4 = (uint32_t)forced.cr + 4u;
                const size_t pay_crc_bytes = (size_t)forced.payload_len + 2u;
                const size_t pay_bits_exact = pay_crc_bytes * 2u * payload_cr_plus4;
                const uint32_t payload_block_bits = (uint32_t)sf * payload_cr_plus4;
                size_t pay_bits_padded = pay_bits_exact;
                if (pay_bits_padded % payload_block_bits) pay_bits_padded = ((pay_bits_padded / payload_block_bits) + 1) * payload_block_bits;
                size_t hdr_bits_exact = 5u * 2u * 8u; // 80
                uint32_t header_block_bits = (uint32_t)sf * 8u;
                size_t hdr_bits_padded = hdr_bits_exact;
                if (hdr_bits_padded % header_block_bits) hdr_bits_padded = ((hdr_bits_padded / header_block_bits) + 1) * header_block_bits;
                size_t hdr_nsym = hdr_bits_padded / (size_t)sf;
                size_t pay_nsym = pay_bits_padded / (size_t)sf;
                if (data.size() >= (hdr_nsym + pay_nsym) * ws.N) {
                    ws.ensure_rx_buffers(pay_nsym, (uint32_t)sf, payload_cr_plus4);
                    auto& symbols_pay = ws.rx_symbols; symbols_pay.resize(pay_nsym);
                    for (size_t s = 0; s < pay_nsym; ++s) {
                        uint32_t raw_symbol = demod_symbol_local(ws, &data[(hdr_nsym + s) * ws.N]);
                        symbols_pay[s] = lora::utils::gray_encode(raw_symbol);
                    }
                    auto& bits_pay = ws.rx_bits; bits_pay.resize(pay_nsym * (size_t)sf);
                    size_t bix = 0;
                    for (size_t s = 0; s < pay_nsym; ++s) { uint32_t sym = symbols_pay[s]; for (int b = sf - 1; b >= 0; --b) bits_pay[bix++] = (sym >> b) & 1u; }
                    const auto& Mp = ws.get_interleaver((uint32_t)sf, payload_cr_plus4);
                    auto& deint_pay = ws.rx_deint; deint_pay.resize(bix);
                    for (size_t off = 0; off + Mp.n_in <= bix; off += Mp.n_in)
                        for (uint32_t i = 0; i < Mp.n_out; ++i)
                            deint_pay[off + Mp.map[i]] = bits_pay[off + i];
                    static lora::utils::HammingTables Tpay = lora::utils::make_hamming_tables();
                    std::vector<uint8_t> nibbles_pay; nibbles_pay.reserve(pay_crc_bytes * 2);
                    bool fec_failed = false;
                    for (size_t i = 0; i < pay_bits_exact; i += payload_cr_plus4) {
                        uint16_t cw = 0; for (uint32_t b = 0; b < payload_cr_plus4; ++b) cw = (cw << 1) | deint_pay[i + b];
                        auto dec = lora::utils::hamming_decode4(cw, payload_cr_plus4, forced.cr, Tpay);
                        if (!dec) { fec_failed = true; nibbles_pay.push_back(0u); }
                        else { nibbles_pay.push_back(dec->first & 0x0F); }
                        if (nibbles_pay.size() >= pay_crc_bytes * 2) break;
                    }
                    std::vector<uint8_t> pay(pay_crc_bytes, 0);
                    for (size_t i = 0; i < pay_crc_bytes; ++i) {
                        uint8_t low  = (i*2   < nibbles_pay.size()) ? nibbles_pay[i*2]     : 0u;
                        uint8_t high = (i*2+1 < nibbles_pay.size()) ? nibbles_pay[i*2 + 1] : 0u;
                        pay[i] = (uint8_t)((high << 4) | low);
                    }
                    // Save to logs for comparison
                    std::filesystem::create_directories("logs");
                    { std::ofstream f("logs/lite_rx_predew.bin", std::ios::binary); if (f) f.write((const char*)pay.data(), (std::streamsize)pay.size()); }
                    auto pay_dw = pay; { auto lfsr2 = lora::utils::LfsrWhitening::pn9_default(); if (forced.payload_len > 0) lfsr2.apply(pay_dw.data(), forced.payload_len); }
                    { std::ofstream f("logs/lite_rx_postdew.bin", std::ios::binary); if (f) f.write((const char*)pay_dw.data(), (std::streamsize)pay_dw.size()); }
                    lora::utils::Crc16Ccitt c;
                    uint8_t crc_lo = pay[forced.payload_len]; uint8_t crc_hi = pay[forced.payload_len + 1];
                    uint16_t crc_rx_le = (uint16_t)crc_lo | ((uint16_t)crc_hi << 8);
                    uint16_t crc_rx_be = ((uint16_t)crc_hi << 8) | (uint16_t)crc_lo;
                    uint16_t crc_calc = c.compute(pay_dw.data(), forced.payload_len);
                    bool crc_ok_le = (crc_calc == crc_rx_le);
                    bool crc_ok_be = (crc_calc == crc_rx_be);
                    if (json) {
                        auto tohex = [](const std::vector<uint8_t>& v){ static const char* H="0123456789abcdef"; std::string s; s.reserve(v.size()*3); for (size_t i=0;i<v.size();++i){ uint8_t b=v[i]; s.push_back(H[(b>>4)&0xF]); s.push_back(H[b&0xF]); if (i+1<v.size()) s.push_back(' ');} return s; };
                        std::fprintf(stdout,
                            "{\n  \"hdr\": {\"sf\":%d,\"cr_hdr\":\"4/8\",\"cr_payload\":\"4/%d\",\"payload_len\":%u},\n  \"predew_hex\": \"%s\",\n  \"postdew_hex\": \"%s\",\n  \"crc_calc\": \"%04x\",\n  \"crc_rx_le\": \"%04x\",\n  \"crc_rx_be\": \"%04x\",\n  \"crc_ok_le\": %s, \"crc_ok_be\": %s\n}\n",
                            sf, (int)forced.cr + 4, (unsigned)forced.payload_len,
                            tohex(pay).c_str(), tohex(pay_dw).c_str(), (unsigned)crc_calc, (unsigned)crc_rx_le, (unsigned)crc_rx_be,
                            crc_ok_le?"true":"false", crc_ok_be?"true":"false");
                        return 0;
                    }
                }
            }
        }
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
                // If payload diagnostics (A3/A4) are available, emit them regardless of reason
                if (!ws.dbg_postdew.empty()) {
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
                        "{\n  \"hdr\": {\"sf\":%d,\"cr_hdr\":\"4/8\",\"cr_payload\":\"4/%d\",\"payload_len\":%u},\n  \"predew_hex\": \"%s\",\n  \"postdew_hex\": \"%s\",\n  \"crc_calc\": \"%04x\",\n  \"crc_rx_le\": \"%04x\",\n  \"crc_rx_be\": \"%04x\",\n  \"crc_ok_le\": %s, \"crc_ok_be\": %s,\n  \"crc_calc_init0000\": \"%04x\", \"crc_ok_le_init0000\": %s, \"crc_ok_be_init0000\": %s,\n  \"crc_calc_refboth\": \"%04x\", \"crc_ok_le_refboth\": %s, \"crc_ok_be_refboth\": %s,\n  \"crc_calc_xorffff\": \"%04x\", \"crc_ok_le_xorffff\": %s, \"crc_ok_be_xorffff\": %s\n}\n",
                        sf, cr_denom, (unsigned)ws.dbg_payload_len,
                        to_hex_str(ws.dbg_predew).c_str(), to_hex_str(ws.dbg_postdew).c_str(),
                        (unsigned)ws.dbg_crc_calc, (unsigned)ws.dbg_crc_rx_le, (unsigned)ws.dbg_crc_rx_be,
                        ws.dbg_crc_ok_le?"true":"false", ws.dbg_crc_ok_be?"true":"false",
                        (unsigned)ws.dbg_crc_calc_init0000, ws.dbg_crc_ok_le_init0000?"true":"false", ws.dbg_crc_ok_be_init0000?"true":"false",
                        (unsigned)ws.dbg_crc_calc_refboth, ws.dbg_crc_ok_le_refboth?"true":"false", ws.dbg_crc_ok_be_refboth?"true":"false",
                        (unsigned)ws.dbg_crc_calc_xorffff, ws.dbg_crc_ok_le_xorffff?"true":"false", ws.dbg_crc_ok_be_xorffff?"true":"false"
                    );
                    return code;
                }

                auto hdr2 = lora::rx::decode_header_with_preamble_cfo_sto_os(ws, span, static_cast<uint32_t>(sf), cr, static_cast<size_t>(min_pre), static_cast<uint8_t>(sync_hex & 0xFF));
                if (hdr2.has_value()) {
                    // If payload diagnostics (A3/A4) are available, emit them regardless of reason
                    if (!ws.dbg_postdew.empty()) {
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
                            "{\n  \"hdr\": {\"sf\":%d,\"cr_hdr\":\"4/8\",\"cr_payload\":\"4/%d\",\"payload_len\":%u},\n  \"predew_hex\": \"%s\",\n  \"postdew_hex\": \"%s\",\n  \"crc_calc\": \"%04x\",\n  \"crc_rx_le\": \"%04x\",\n  \"crc_rx_be\": \"%04x\",\n  \"crc_ok_le\": %s, \"crc_ok_be\": %s,\n  \"crc_calc_init0000\": \"%04x\", \"crc_ok_le_init0000\": %s, \"crc_ok_be_init0000\": %s,\n  \"crc_calc_refboth\": \"%04x\", \"crc_ok_le_refboth\": %s, \"crc_ok_be_refboth\": %s,\n  \"crc_calc_xorffff\": \"%04x\", \"crc_ok_le_xorffff\": %s, \"crc_ok_be_xorffff\": %s\n}\n",
                            sf, cr_denom, (unsigned)ws.dbg_payload_len,
                            to_hex_str(ws.dbg_predew).c_str(), to_hex_str(ws.dbg_postdew).c_str(),
                            (unsigned)ws.dbg_crc_calc, (unsigned)ws.dbg_crc_rx_le, (unsigned)ws.dbg_crc_rx_be,
                            ws.dbg_crc_ok_le?"true":"false", ws.dbg_crc_ok_be?"true":"false",
                            (unsigned)ws.dbg_crc_calc_init0000, ws.dbg_crc_ok_le_init0000?"true":"false", ws.dbg_crc_ok_be_init0000?"true":"false",
                            (unsigned)ws.dbg_crc_calc_refboth, ws.dbg_crc_ok_le_refboth?"true":"false", ws.dbg_crc_ok_be_refboth?"true":"false",
                            (unsigned)ws.dbg_crc_calc_xorffff, ws.dbg_crc_ok_le_xorffff?"true":"false", ws.dbg_crc_ok_be_xorffff?"true":"false"
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
