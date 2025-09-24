// Optimized and modernized the header file for better readability and maintainability.
#pragma once

#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
#include <optional> // For modern C++ optional handling

// Logging macro
#define DEBUGF(fmt, ...) std::printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)

// IQ type
using cfloat = std::complex<float>;

// Global constants
constexpr size_t MAX_RAW_SAMPLES = 4 * 1024 * 1024; // 4M samples (~32MB)
constexpr int    GUARD_SYMS      = 1;               // Overlap after frame
constexpr int    FAIL_STEP_DIV   = 8;               // Minimal advance in failure

struct RxConfig {
    uint8_t sf;     // Spreading factor (7..12)
    uint32_t os;    // Oversampling (1/2/4/8)
    bool ldro;      // Low data rate optimization
    uint8_t cr_idx; // Coding rate index (1=CR45, 2=CR46, 3=CR47, 4=CR48)
    float bandwidth_hz; // Bandwidth in Hz (125000, 250000, 500000)
};

inline size_t N_per_symbol(uint8_t sf) { return size_t(1u) << sf; }

inline size_t dec_syms_to_raw_samples(size_t syms, const RxConfig& cfg) {
    return syms * N_per_symbol(cfg.sf) * cfg.os;
}

struct Ring {
    std::unique_ptr<cfloat[]> buf;
    size_t capacity_ = MAX_RAW_SAMPLES;
    size_t head = 0;   // Read index (RAW)
    size_t tail = 0;   // Write index (RAW)

    Ring() { buf = std::make_unique<cfloat[]>(capacity_); }

    size_t avail() const { return tail - head; }
    bool   have(size_t need) const { return avail() >= need; }
    size_t capacity() const { return capacity_; }

    const cfloat* ptr(size_t raw_idx) const {
        return (raw_idx < capacity_) ? &buf[raw_idx] : nullptr;
    }
    cfloat* wptr(size_t raw_idx) {
        return (raw_idx < capacity_) ? &buf[raw_idx] : nullptr;
    }

    void advance(size_t raw) { head = std::min(head + raw, tail); }
    bool can_write(size_t amount) const { return tail + amount <= capacity_; }
};

struct DetectPreambleResult {
    bool   found = false;
    size_t preamble_start_raw = 0;
    uint32_t os = 1;
    float mu = 0.f;
    float eps = 0.f;
    int   cfo_int = 0;
    int   phase = 0;
    float cfo_estimate = 0.f;
    int   sto_estimate = 0;
};

struct LocateHeaderResult {
    bool   ok = false;
    size_t header_start_raw = 0;
};

struct HeaderResult {
    bool   ok = false;
    uint8_t sf = 7;
    uint8_t cr_idx = 1;
    bool ldro = false;
    bool has_crc = true;
    uint16_t payload_len_bytes = 0;
    size_t header_syms = 16;
    uint32_t detected_os = 0;
    int detected_phase = 0;
    size_t det_start_raw = 0;
    size_t start_decim = 0;
    size_t preamble_start_decim = 0;
    size_t aligned_start_decim = 0;
    size_t header_start_decim = 0;
    size_t sync_start_decim = 0;
    float fractional_cfo = 0.0f;
    int sto = 0;

    HeaderResult(bool ok, uint8_t sf, uint8_t cr_idx, bool ldro, bool has_crc, uint16_t payload_len_bytes, size_t header_syms, uint32_t detected_os, int detected_phase, size_t det_start_raw, size_t start_decim, size_t preamble_start_decim, size_t aligned_start_decim, size_t header_start_decim, size_t sync_start_decim, float fractional_cfo, int sto)
        : ok(ok), sf(sf), cr_idx(cr_idx), ldro(ldro), has_crc(has_crc), payload_len_bytes(payload_len_bytes), header_syms(header_syms), detected_os(detected_os), detected_phase(detected_phase), det_start_raw(det_start_raw), start_decim(start_decim), preamble_start_decim(preamble_start_decim), aligned_start_decim(aligned_start_decim), header_start_decim(header_start_decim), sync_start_decim(sync_start_decim), fractional_cfo(fractional_cfo), sto(sto) {}
    HeaderResult() = default;
};

struct PayloadResult {
    bool   ok = false;
    size_t payload_syms = 0;
    size_t consumed_raw = 0;
    bool   crc_ok = false;
    std::vector<uint8_t> payload_data;
};

DetectPreambleResult detect_preamble_dynamic(const cfloat* raw, size_t raw_len, const RxConfig& cfg, size_t history_raw);
LocateHeaderResult   locate_header_start(const cfloat* raw, size_t raw_len, const RxConfig& cfg, const DetectPreambleResult& d);
HeaderResult         demod_header(const cfloat* raw, size_t raw_len, const RxConfig& cfg, const LocateHeaderResult& h, const DetectPreambleResult& d);
size_t               expected_payload_symbols(uint16_t pay_len, uint8_t cr_idx, bool ldro, uint8_t sf);
PayloadResult        demod_payload(const cfloat* raw, size_t raw_len, const RxConfig& cfg, const HeaderResult& hdr, const DetectPreambleResult& d, size_t header_start_raw);

enum class RxState { SEARCH_PREAMBLE, LOCATE_HEADER, DEMOD_HEADER, DEMOD_PAYLOAD, YIELD_FRAME, ADVANCE };

struct FrameCtx {
    DetectPreambleResult d;
    LocateHeaderResult   l;
    HeaderResult         h;
    PayloadResult        p;
    size_t frame_start_raw = 0;
    size_t frame_end_raw   = 0;
};

struct Scheduler {
    RxConfig cfg;
    size_t H_raw = 0;
    size_t W_raw = 0;
    RxState st = RxState::SEARCH_PREAMBLE;
    FrameCtx ctx{};
    size_t small_fail_step_raw = 0;
    bool frame_ready = false;
    FrameCtx last_frame{};

    void init(const RxConfig& c) {
        cfg = c;
        H_raw = dec_syms_to_raw_samples(8, cfg);
        W_raw = dec_syms_to_raw_samples(64, cfg);
        small_fail_step_raw = dec_syms_to_raw_samples(1, cfg) / FAIL_STEP_DIV;
        frame_ready = false;
        last_frame = FrameCtx{};
    }

    bool step(Ring& r);
};

std::vector<FrameCtx> run_pipeline_offline(const cfloat* iq, size_t iq_len, const RxConfig& cfg);
