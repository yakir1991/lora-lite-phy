#pragma once

#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

// logging (can be replaced with existing macro)
#define DEBUGF(fmt, ...) std::printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)

// IQ type (according to existing code)
using cfloat = std::complex<float>;

// Global constants (no runtime allocations)
constexpr size_t MAX_RAW_SAMPLES = 4 * 1024 * 1024; // 4M samples (~32MB) reasonable for production
constexpr int    GUARD_SYMS      = 1;                // overlap after frame
constexpr int    FAIL_STEP_DIV   = 8;                // minimal advance in failure

struct RxConfig {
    uint8_t sf;     // 7..12
    uint32_t os;    // oversampling (1/2/4/8)
    bool ldro;      // low data rate optimize
    uint8_t cr_idx; // coding rate index (1=CR45, 2=CR46, 3=CR47, 4=CR48)
    float bandwidth_hz; // bandwidth in Hz (125000, 250000, 500000)
};

inline size_t N_per_symbol(uint8_t sf) { return size_t(1u) << sf; }

// Convert symbols (DEC) → RAW
inline size_t dec_syms_to_raw_samples(size_t syms, const RxConfig& cfg) {
    return syms * N_per_symbol(cfg.sf) * cfg.os;
}

// Ring buffer with heap allocation
struct Ring {
    std::unique_ptr<cfloat[]> buf;
    size_t capacity_;
    size_t head = 0;   // read index (RAW)
    size_t tail = 0;   // write index (RAW)

    Ring() : capacity_(MAX_RAW_SAMPLES) {
        buf = std::make_unique<cfloat[]>(capacity_);
    }

    size_t avail() const { return tail - head; }
    bool   have(size_t need) const { return avail() >= need; }
    size_t capacity() const { return capacity_; }

    // Span access without allocation/copy - with bounds checking
    const cfloat* ptr(size_t raw_idx) const {
        return (raw_idx < capacity_) ? &buf[raw_idx] : nullptr;
    }
    cfloat* wptr(size_t raw_idx) {
        return (raw_idx < capacity_) ? &buf[raw_idx] : nullptr;
    }

    // Safe advance
    void advance(size_t raw) { head += raw; if (head > tail) head = tail; }

    // Check if we can write 'amount' more samples
    bool can_write(size_t amount) const { return tail + amount <= capacity_; }
};

// Temporary detection/demod results (what already returned)
struct DetectPreambleResult {
    bool   found = false;
    size_t preamble_start_raw = 0; // always RAW
    uint32_t os = 1;
    float mu = 0.f;   // phase/time fractional
    float eps = 0.f;  // CFO fractional
    int   cfo_int = 0; // CFO integer (bins)
    int   phase = 0;   // detection phase
    float cfo_estimate = 0.f; // CFO estimate from preamble
    int   sto_estimate = 0;   // STO estimate from preamble
};

struct LocateHeaderResult {
    bool   ok = false;
    size_t header_start_raw = 0; // RAW index to header start
};

struct HeaderResult {
    bool   ok = false;
    uint8_t sf = 7;
    uint8_t cr_idx = 1;  // 1→4/5, 2→4/6, ...
    bool ldro = false;
    bool has_crc = true;
    uint16_t payload_len_bytes = 0;
    size_t header_syms = 16; // always 16 in LoRa explicit
};

struct PayloadResult {
    bool   ok = false;
    size_t payload_syms = 0;     // DEC
    size_t consumed_raw = 0;     // how much RAW consumed actually
    bool   crc_ok = false;
    std::vector<uint8_t> payload_data; // decoded payload bytes
};

// Declarations for existing functions (keep existing names)
DetectPreambleResult detect_preamble_dynamic(const cfloat* raw, size_t raw_len, const RxConfig& cfg, size_t history_raw);
LocateHeaderResult   locate_header_start(const cfloat* raw, size_t raw_len, const RxConfig& cfg, const DetectPreambleResult& d);
HeaderResult         demod_header(const cfloat* raw, size_t raw_len, const RxConfig& cfg, const LocateHeaderResult& h, const DetectPreambleResult& d);
size_t               expected_payload_symbols(uint16_t pay_len, uint8_t cr_idx, bool ldro, uint8_t sf); // exists
PayloadResult        demod_payload(const cfloat* raw, size_t raw_len, const RxConfig& cfg,
                                   const HeaderResult& hdr, const DetectPreambleResult& d, size_t header_start_raw);

// State machine + history + forecast + advance policy
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
    size_t H_raw; // history RAW samples
    size_t W_raw; // coarse search window

    RxState st = RxState::SEARCH_PREAMBLE;
    FrameCtx ctx{};
    size_t small_fail_step_raw = 0; // (N*OS)/8
    bool frame_ready = false;
    FrameCtx last_frame{};

    void init(const RxConfig& c) {
        cfg = c;
        H_raw = dec_syms_to_raw_samples(8, cfg); // history 8 symbols (preamble length)
        W_raw = dec_syms_to_raw_samples(64, cfg);// search window (preamble + sync + header + some payload)
        small_fail_step_raw = dec_syms_to_raw_samples(1, cfg) / FAIL_STEP_DIV;
        frame_ready = false;
        last_frame = FrameCtx{};
    }

    bool step(Ring& r) {
        if (r.avail() < (H_raw + W_raw)) {
            DEBUGF("not enough samples: avail=%zu need>=%zu", r.avail(), H_raw+W_raw);
            return false; // wait for more (or end offline)
        }
        // Window with history: start reading from head-H (careful not to go below 0)
        const size_t win_head = (r.head >= H_raw) ? (r.head - H_raw) : r.head;
        const size_t win_len  = std::min(r.avail() + (r.head - win_head), H_raw + W_raw);
        const cfloat* raw     = r.ptr(win_head);

        if (!raw) {
            DEBUGF("Invalid read pointer at win_head %zu (head=%zu, H_raw=%zu)", win_head, r.head, H_raw);
            return false;
        }

        if (win_len == 0) {
            DEBUGF("Empty window: win_len=0");
            return false;
        }

        switch (st) {
        case RxState::SEARCH_PREAMBLE: {
            ctx = FrameCtx{}; // reset frame context
            ctx.d = detect_preamble_dynamic(raw, win_len, cfg, H_raw);
            DEBUGF("SEARCH: head=%zu win=[%zu..%zu) found=%d os=%u mu=%.3f eps=%.3f cfo_int=%d",
                   r.head, win_head, win_head+win_len, int(ctx.d.found), ctx.d.os, ctx.d.mu, ctx.d.eps, ctx.d.cfo_int);
            st = ctx.d.found ? RxState::LOCATE_HEADER : RxState::ADVANCE;
            break;
        }
        case RxState::LOCATE_HEADER: {
            ctx.l = locate_header_start(raw, win_len, cfg, ctx.d);
            DEBUGF("LOCATE: ok=%d header_start_raw=%zu (abs=%zu)", int(ctx.l.ok),
                   ctx.l.header_start_raw, win_head + ctx.l.header_start_raw);
            st = ctx.l.ok ? RxState::DEMOD_HEADER : RxState::ADVANCE;
            break;
        }
        case RxState::DEMOD_HEADER: {
            ctx.h = demod_header(raw + ctx.l.header_start_raw, win_len - ctx.l.header_start_raw, cfg, ctx.l, ctx.d);
            DEBUGF("HDR: ok=%d sf=%u cr=%u ldro=%d has_crc=%d pay_len=%u",
                   int(ctx.h.ok), ctx.h.sf, ctx.h.cr_idx, int(ctx.h.ldro), int(ctx.h.has_crc), ctx.h.payload_len_bytes);
            if (!ctx.h.ok) { st = RxState::ADVANCE; break; }

            ctx.frame_start_raw = win_head + ctx.l.header_start_raw; // absolute RAW
            st = RxState::DEMOD_PAYLOAD;
            break;
        }
        case RxState::DEMOD_PAYLOAD: {
            const size_t local_off = ctx.l.header_start_raw;
            ctx.p = demod_payload(raw + local_off, win_len - local_off, cfg, ctx.h, ctx.d, ctx.frame_start_raw);
            DEBUGF("PAY: ok=%d payload_syms=%zu consumed_raw=%zu crc_ok=%d",
                   int(ctx.p.ok), ctx.p.payload_syms, ctx.p.consumed_raw, int(ctx.p.crc_ok));

            // Account for header length (16 symbols) if demod_payload didn't include consumption:
            const size_t header_raw = dec_syms_to_raw_samples(ctx.h.header_syms, cfg);
            const size_t consumed_raw_total = header_raw + ctx.p.consumed_raw;
            ctx.frame_end_raw = ctx.frame_start_raw + consumed_raw_total;

            st = RxState::YIELD_FRAME;
            break;
        }
        case RxState::YIELD_FRAME: {
            // Here emit JSON/structure - no allocations, just print or write to external buffer
            DEBUGF("EMIT: frame_start=%zu frame_end=%zu len_raw=%zu crc_ok=%d",
                   ctx.frame_start_raw, ctx.frame_end_raw, ctx.frame_end_raw - ctx.frame_start_raw, int(ctx.p.crc_ok));
            last_frame = ctx;
            frame_ready = true;
            st = RxState::ADVANCE;
            break;
        }
        case RxState::ADVANCE: {
            size_t adv = 0;
            if (!ctx.h.ok) {
                // failure - small step
                adv = small_fail_step_raw;
                DEBUGF("ADVANCE: fail_step=%zu", adv);
            } else {
                // success - jump to end of frame minus GUARD (1 symbol)
                const size_t guard_raw = dec_syms_to_raw_samples(GUARD_SYMS, cfg);
                const size_t target    = (ctx.frame_end_raw > guard_raw) ? (ctx.frame_end_raw - guard_raw) : ctx.frame_end_raw;
                adv = (target > r.head) ? (target - r.head) : 0;
                DEBUGF("ADVANCE: success to=%zu (head=%zu) step=%zu", target, r.head, adv);
            }
            r.advance(adv);
            st = RxState::SEARCH_PREAMBLE;
            break;
        }
        }
        return true; // did step
    }
};

// Offline pipeline runner - fed in chunks (also offline)
std::vector<FrameCtx> run_pipeline_offline(const cfloat* iq, size_t iq_len, const RxConfig& cfg);
