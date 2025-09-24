// Optimized and modernized the code for better readability, performance, and maintainability.
#include "lora/rx/lora_scheduler.hpp"

#include "lora/rx/lite/lora_primitives.hpp"
#include "lora/rx/lite/lora_header_decode.hpp"
#include "lora/rx/lite/lora_utils.hpp"
#include "lora/workspace.hpp"
#include <algorithm>
#include <cstring>
#include <vector>
#include <chrono>
#include <optional> // For modern C++ optional handling

// Helper function to calculate payload symbols
size_t calculate_payload_symbols(uint16_t pay_len, uint8_t cr_idx, bool ldro, uint8_t sf) {
    const int SF = sf;
    const int DE = ldro ? 1 : 0;
    const int IH = 0; // explicit header
    const int CRC = 1; // assume CRC present
    const int CR  = cr_idx; // 1..4
    int num = (8 * int(pay_len) - 4 * SF + 28 + 16 * CRC - 20 * IH);
    int den = 4 * (SF - 2 * DE);
    int ceil_term = (num <= 0) ? 0 : ((num + den - 1) / den);
    return 8 + std::max(ceil_term * (CR + 4), 0);
}

// Wrapper for detect_preamble_dynamic
DetectPreambleResult detect_preamble_dynamic(const cfloat* raw, size_t raw_len, const RxConfig& cfg, size_t history_raw) {
    DetectPreambleResult ret;
    std::span<const std::complex<float>> samples(raw, raw_len);

    std::vector<int> candidates = {1, 2, 4, 8};
    lora::Workspace ws;
    ws.init(cfg.sf);

    auto result = lora::rx::gr::detect_preamble_os(ws, samples, cfg.sf, 8, candidates);
    if (result) {
        ret = {result->start_sample, static_cast<uint32_t>(result->os), static_cast<float>(result->phase), 0, 0, 0, 0, 0};
    }
    return ret;
}

// Wrapper for locate_header_start
LocateHeaderResult locate_header_start(const cfloat* raw, size_t raw_len, const RxConfig& cfg, const DetectPreambleResult& d) {
    LocateHeaderResult ret;
    if (!d.found) return ret;

    const size_t preamble_sync_raw = dec_syms_to_raw_samples(15, cfg);
    if (d.preamble_start_raw + preamble_sync_raw < raw_len) {
        ret = {true, d.preamble_start_raw + preamble_sync_raw};
    }
    return ret;
}

// Wrapper for demod_header
HeaderResult demod_header(const cfloat* raw, size_t raw_len, const RxConfig& cfg, const LocateHeaderResult& h, const DetectPreambleResult& d) {
    HeaderResult ret;
    if (!raw || raw_len == 0) return ret;

    std::span<const std::complex<float>> samples(raw, raw_len);
    lora::Workspace ws;
    ws.init(cfg.sf);

    constexpr uint8_t kExpectedSyncWord = 0x34;
    constexpr size_t kMinPreambleSyms = 8;

    auto header = lora::rx::gr::decode_header_with_preamble_cfo_sto_os(ws, samples, cfg.sf, lora::rx::gr::CodeRate::CR45, kMinPreambleSyms, kExpectedSyncWord);
    if (header) {
        ret = HeaderResult(true, cfg.sf, cfg.cr_idx, cfg.ldro, header->has_crc, header->payload_len, 16, static_cast<uint32_t>(header->cr), ws.dbg_hdr_os, ws.dbg_hdr_phase, ws.dbg_hdr_det_start_raw, ws.dbg_hdr_start_decim, ws.dbg_hdr_preamble_start, ws.dbg_hdr_aligned_start, ws.dbg_hdr_header_start, ws.dbg_hdr_sync_start, ws.dbg_hdr_cfo);
    }
    return ret;
}

// Wrapper for demod_payload
PayloadResult demod_payload(const cfloat* raw, size_t raw_len, const RxConfig& cfg, const HeaderResult& hdr, const DetectPreambleResult& d, size_t header_start_raw) {
    PayloadResult ret;
    if (!hdr.ok) return ret;

    ret.payload_syms = calculate_payload_symbols(hdr.payload_len_bytes, hdr.cr_idx, hdr.ldro, hdr.sf);
    const size_t header_raw = dec_syms_to_raw_samples(hdr.header_syms, cfg);
    const size_t payload_raw = dec_syms_to_raw_samples(ret.payload_syms, cfg);

    size_t available_payload = std::min(payload_raw, raw_len > header_raw ? raw_len - header_raw : 0);
    ret.consumed_raw = available_payload;
    ret.ok = false;
    ret.crc_ok = false;
    return ret;
}

// Offline pipeline runner
std::vector<FrameCtx> run_pipeline_offline(const cfloat* iq, size_t iq_len, const RxConfig& cfg) {
    std::vector<FrameCtx> frames;
    if (!iq || iq_len == 0) return frames;

    auto start_time = std::chrono::high_resolution_clock::now();
    Ring ring;
    Scheduler sch;

    try {
        sch.init(cfg);
    } catch (const std::exception& e) {
        DEBUGF("Failed to initialize scheduler: %s", e.what());
        return frames;
    }

    constexpr size_t CHUNK = 64 * 1024;
    size_t pos = 0;

    while (pos < iq_len) {
        size_t push = std::min(CHUNK, iq_len - pos);
        if (!ring.can_write(push)) break;

        cfloat* wptr = ring.wptr(ring.tail);
        if (!wptr) break;

        try {
            std::copy(iq + pos, iq + pos + push, wptr);
            ring.tail += push;
            pos += push;

            while (sch.step(ring)) {
                if (sch.frame_ready) {
                    frames.push_back(sch.last_frame);
                    sch.frame_ready = false;
                }
            }
        } catch (const std::exception& e) {
            DEBUGF("Error processing chunk at pos %zu: %s", pos, e.what());
            break;
        }
    }

    try {
        while (sch.step(ring)) {
            if (sch.frame_ready) {
                frames.push_back(sch.last_frame);
                sch.frame_ready = false;
            }
        }
    } catch (const std::exception& e) {
        DEBUGF("Error in final processing: %s", e.what());
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

    DEBUGF("Pipeline completed: processed %zu/%zu samples", pos, iq_len);
    DEBUGF("Performance: %.2f MSamples/sec", (double)iq_len / (duration.count() / 1e6) / 1e6);
    return frames;
}

bool Scheduler::step(Ring& r) {
    DEBUGF("Scheduler::step called with state: %d, available samples: %zu", static_cast<int>(st), r.avail());

    if (r.avail() < H_raw) {
        DEBUGF("Not enough data to process. Required: %zu, Available: %zu", H_raw, r.avail());
        return false; // Not enough data to process
    }

    switch (st) {
        case RxState::SEARCH_PREAMBLE: {
            DEBUGF("State: SEARCH_PREAMBLE");
            auto d = detect_preamble_dynamic(r.ptr(r.head), r.avail(), cfg, H_raw);
            if (d.found) {
                DEBUGF("Preamble found at raw index: %zu", d.preamble_start_raw);
                ctx.d = d;
                ctx.frame_start_raw = d.preamble_start_raw;
                st = RxState::LOCATE_HEADER;
            } else {
                DEBUGF("Preamble not found. Advancing by: %zu", small_fail_step_raw);
                r.advance(small_fail_step_raw);
            }
            break;
        }
        case RxState::LOCATE_HEADER: {
            DEBUGF("State: LOCATE_HEADER");
            auto l = locate_header_start(r.ptr(r.head), r.avail(), cfg, ctx.d);
            if (l.ok) {
                DEBUGF("Header located at raw index: %zu", l.header_start_raw);
                ctx.l = l;
                st = RxState::DEMOD_HEADER;
            } else {
                DEBUGF("Header not located. Returning to SEARCH_PREAMBLE.");
                r.advance(small_fail_step_raw);
                st = RxState::SEARCH_PREAMBLE;
            }
            break;
        }
        case RxState::DEMOD_HEADER: {
            DEBUGF("State: DEMOD_HEADER");
            auto h = demod_header(r.ptr(r.head), r.avail(), cfg, ctx.l, ctx.d);
            if (h.ok) {
                DEBUGF("Header decoded successfully. Payload length: %u", h.payload_len_bytes);
                ctx.h = h;
                st = RxState::DEMOD_PAYLOAD;
            } else {
                DEBUGF("Header decoding failed. Returning to SEARCH_PREAMBLE.");
                r.advance(small_fail_step_raw);
                st = RxState::SEARCH_PREAMBLE;
            }
            break;
        }
        case RxState::DEMOD_PAYLOAD: {
            DEBUGF("State: DEMOD_PAYLOAD");
            auto p = demod_payload(r.ptr(r.head), r.avail(), cfg, ctx.h, ctx.d, ctx.l.header_start_raw);
            if (p.ok) {
                DEBUGF("Payload decoded successfully. Consumed raw samples: %zu", p.consumed_raw);
                ctx.p = p;
                ctx.frame_end_raw = ctx.frame_start_raw + p.consumed_raw;
                frame_ready = true;
                last_frame = ctx;
                r.advance(ctx.frame_end_raw - r.head);
                st = RxState::SEARCH_PREAMBLE;
            } else {
                DEBUGF("Payload decoding failed. Returning to SEARCH_PREAMBLE.");
                r.advance(small_fail_step_raw);
                st = RxState::SEARCH_PREAMBLE;
            }
            break;
        }
        default:
            DEBUGF("Unknown state: %d", static_cast<int>(st));
            return false;
    }
    return true;
}

size_t expected_payload_symbols(uint16_t pay_len, uint8_t cr_idx, bool ldro, uint8_t sf) {
    // Stub implementation
    return 0;
}
