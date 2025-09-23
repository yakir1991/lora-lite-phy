#include "lora/rx/scheduler.hpp"

#include <algorithm>
#include <cstring>
#include <vector>
#include <chrono>
#include <string>
#include <span>
#include <limits>

#include "lora/rx/gr/primitives.hpp"
#include "lora/rx/gr/header_decode.hpp"
#include "lora/rx/gr/utils.hpp"
// #include "lora/rx/gr/payload_decode.hpp" // Not available yet
// Removed gr_pipeline.hpp dependency - scheduler is independent!
#include "lora/workspace.hpp"

// Wrapper for existing detect_preamble_dynamic
DetectPreambleResult detect_preamble_dynamic(const cfloat* raw, size_t raw_len, const RxConfig& cfg, size_t history_raw) {
    DetectPreambleResult ret;
    
    // Create span from raw pointer
    std::span<const std::complex<float>> samples(raw, raw_len);

    // Use existing function with candidates based on cfg.os
    std::vector<int> candidates;
    if (cfg.os == 1) candidates = {1};
    else if (cfg.os == 2) candidates = {2, 1};
    else if (cfg.os == 4) candidates = {4, 2, 1};
    else if (cfg.os == 8) candidates = {8, 4, 2, 1};
    else candidates = {1, 2, 4, 8};

    // Use the gr version for preamble detection (independent of pipeline)
    lora::Workspace ws;
    ws.init(cfg.sf);
    auto result = lora::rx::gr::detect_preamble_os(ws, samples, cfg.sf, 8, candidates);

    if (result) {
        ret.found = true;
        ret.preamble_start_raw = result->start_sample;
        ret.os = static_cast<uint32_t>(result->os);
        ret.phase = result->phase;
        
        // Try to estimate CFO and STO from the preamble
        // This is a simplified version - real implementation would use the full pipeline
        ret.mu = 0.f; // timing offset - would need STO estimation
        ret.eps = 0.f; // CFO fractional - would need CFO estimation
        ret.cfo_int = 0; // CFO integer bins
        ret.cfo_estimate = 0.f; // placeholder
        ret.sto_estimate = 0; // placeholder
    } else {
        ret.found = false;
        ret.preamble_start_raw = 0;
        ret.os = cfg.os;
        ret.phase = 0;
        ret.mu = 0.f;
        ret.eps = 0.f;
        ret.cfo_int = 0;
        ret.cfo_estimate = 0.f;
        ret.sto_estimate = 0;
    }
    
    return ret;
}

// Wrapper for locate_header_start - simplified version
LocateHeaderResult locate_header_start(const cfloat* raw, size_t raw_len, const RxConfig& cfg, const DetectPreambleResult& d) {
    LocateHeaderResult ret;

    if (!d.found) return ret;

    const uint32_t os = std::max<uint32_t>(cfg.os, 1u);
    const size_t preamble_raw = dec_syms_to_raw_samples(8, cfg);
    const size_t sync_raw = dec_syms_to_raw_samples(2, cfg);
    const size_t delimiter_raw = dec_syms_to_raw_samples(2, cfg) + (N_per_symbol(cfg.sf) * os) / 4u;

    size_t header_start_raw = d.preamble_start_raw;
    const size_t offsets[] = {preamble_raw, sync_raw, delimiter_raw};
    for (size_t off : offsets) {
        if (off > std::numeric_limits<size_t>::max() - header_start_raw) {
            return ret; // overflow guard
        }
        header_start_raw += off;
    }

    if (header_start_raw < raw_len) {
        ret.ok = true;
        ret.header_start_raw = header_start_raw;
    }

    return ret;
}

// Wrapper for demod_header - using real header decoding
HeaderResult demod_header(const cfloat* raw, size_t raw_len, const RxConfig& cfg, const LocateHeaderResult& h, const DetectPreambleResult& d) {
    (void)h;
    (void)d;

    HeaderResult ret;

    if (!raw || raw_len == 0) return ret;

    std::span<const std::complex<float>> samples(raw, raw_len);

    lora::Workspace ws;
    ws.init(cfg.sf);

    constexpr uint8_t kExpectedSyncWord = 0x34;
    constexpr size_t kMinPreambleSyms = 8;

    auto header = lora::rx::gr::decode_header_with_preamble_cfo_sto_os(
        ws,
        samples,
        cfg.sf,
        lora::rx::gr::CodeRate::CR45,
        kMinPreambleSyms,
        kExpectedSyncWord
    );

    if (!header) {
        DEBUGF("Header decode failed");
        return ret;
    }

    ret.ok = true;
    ret.sf = cfg.sf;
    ret.ldro = cfg.ldro;
    ret.has_crc = header->has_crc;
    ret.payload_len_bytes = header->payload_len;
    ret.header_syms = 16;

    switch (header->cr) {
        case lora::rx::gr::CodeRate::CR45: ret.cr_idx = 1; break;
        case lora::rx::gr::CodeRate::CR46: ret.cr_idx = 2; break;
        case lora::rx::gr::CodeRate::CR47: ret.cr_idx = 3; break;
        case lora::rx::gr::CodeRate::CR48: ret.cr_idx = 4; break;
        default: ret.cr_idx = cfg.cr_idx; break;
    }

    ret.detected_os = static_cast<uint32_t>(ws.dbg_hdr_os);
    ret.detected_phase = ws.dbg_hdr_phase;
    ret.det_start_raw = ws.dbg_hdr_det_start_raw;
    ret.start_decim = ws.dbg_hdr_start_decim;
    ret.preamble_start_decim = ws.dbg_hdr_preamble_start;
    ret.aligned_start_decim = ws.dbg_hdr_aligned_start;
    ret.header_start_decim = ws.dbg_hdr_header_start;
    ret.sync_start_decim = ws.dbg_hdr_sync_start;
    ret.fractional_cfo = ws.dbg_hdr_cfo;
    ret.sto = ws.dbg_hdr_sto;

    DEBUGF("HDR: ok=%d sf=%u cr=%u ldro=%d has_crc=%d pay_len=%u",
           int(ret.ok), ret.sf, ret.cr_idx, int(ret.ldro), int(ret.has_crc), ret.payload_len_bytes);

    return ret;
}

// expected_payload_symbols - from existing code
size_t expected_payload_symbols(uint16_t pay_len, uint8_t cr_idx, bool ldro, uint8_t sf,
                                bool has_crc, bool implicit_header) {
    // LoRa: nb_sym_pay = 8 + max( ceil( (8*PL - 4*SF + 28 + 16*CRC - 20*IH) / (4*(SF - 2*DE)) ) * (CR+4), 0 )
    const int SF = sf;
    const int DE = ldro ? 1 : 0;
    const int IH = implicit_header ? 1 : 0; // 0 = explicit header
    const int CRC = has_crc ? 1 : 0;
    const int CR  = cr_idx; // 1..4
    int num = (8*int(pay_len) - 4*SF + 28 + 16*CRC - 20*IH);
    int den = 4 * (SF - 2*DE);
    int ceil_term = (num <= 0) ? 0 : ((num + den - 1) / den);
    int nb_sym_pay = 8 + std::max(ceil_term * (CR + 4), 0);
    return size_t(nb_sym_pay);
}

// Wrapper for demod_payload - using real payload decoding
PayloadResult demod_payload(const cfloat* raw, size_t raw_len, const RxConfig& cfg,
                           const HeaderResult& hdr, const DetectPreambleResult& d, size_t header_start_raw) {
    (void)d;
    (void)header_start_raw;

    PayloadResult ret;

    if (!raw || !hdr.ok) {
        return ret;
    }

    ret.payload_syms = expected_payload_symbols(hdr.payload_len_bytes, hdr.cr_idx, hdr.ldro, hdr.sf,
                                               hdr.has_crc);

    const size_t samples_per_symbol = N_per_symbol(cfg.sf) * std::max<uint32_t>(cfg.os, 1u);
    const size_t header_raw = dec_syms_to_raw_samples(hdr.header_syms, cfg);
    const size_t payload_raw = samples_per_symbol * ret.payload_syms;
    const size_t total_needed = header_raw + payload_raw;

    if (raw_len < total_needed) {
        DEBUGF("PAYLOAD FAIL: insufficient samples (have %zu, need %zu)", raw_len, total_needed);
        return ret;
    }

    const cfloat* payload_start = raw + header_raw;

    lora::Workspace ws;
    ws.init(cfg.sf);
    const uint32_t N = ws.N;

    std::vector<std::complex<float>> symbol_block(N);
    std::vector<uint32_t> bins;
    bins.reserve(ret.payload_syms);

    const uint32_t os = std::max<uint32_t>(cfg.os, 1u);
    for (size_t sym = 0; sym < ret.payload_syms; ++sym) {
        const cfloat* sym_ptr = payload_start + sym * samples_per_symbol;
        for (uint32_t n = 0; n < N; ++n) {
            std::complex<float> acc{0.f, 0.f};
            const size_t base = static_cast<size_t>(n) * os;
            for (uint32_t k = 0; k < os; ++k) {
                acc += sym_ptr[base + k];
            }
            symbol_block[n] = acc / static_cast<float>(os);
        }
        uint32_t bin = lora::rx::gr::demod_symbol_peak(ws, symbol_block.data());
        bin &= (N - 1u);
        bins.push_back(bin);
    }

    const uint32_t cw_len = 4u + static_cast<uint32_t>(hdr.cr_idx);
    if (cw_len == 0u) {
        return ret;
    }

    const auto inter_map = lora::rx::gr::make_diagonal_interleaver(cfg.sf, cw_len);
    static const auto tables = lora::rx::gr::make_hamming_tables();

    lora::rx::gr::CodeRate code_rate = lora::rx::gr::CodeRate::CR45;
    switch (hdr.cr_idx) {
        case 1: code_rate = lora::rx::gr::CodeRate::CR45; break;
        case 2: code_rate = lora::rx::gr::CodeRate::CR46; break;
        case 3: code_rate = lora::rx::gr::CodeRate::CR47; break;
        case 4: code_rate = lora::rx::gr::CodeRate::CR48; break;
        default: break;
    }

    const size_t blocks = (bins.size() + cw_len - 1u) / cw_len;
    std::vector<uint8_t> inter(inter_map.n_in);
    std::vector<uint8_t> deinter(inter_map.n_out);
    std::vector<uint8_t> nibbles;
    nibbles.reserve(blocks * static_cast<size_t>(cfg.sf));

    const uint32_t bit_mask = (cfg.sf >= 32u) ? 0xFFFFFFFFu : ((uint32_t(1) << cfg.sf) - 1u);
    for (size_t block = 0; block < blocks; ++block) {
        std::fill(inter.begin(), inter.end(), 0u);
        for (uint32_t col = 0; col < cw_len; ++col) {
            size_t sym_idx = block * cw_len + col;
            uint32_t sym_bits = 0u;
            if (sym_idx < bins.size()) {
                sym_bits = lora::rx::gr::gray_decode(bins[sym_idx]) & bit_mask;
            }
            for (uint32_t row = 0; row < cfg.sf; ++row) {
                uint8_t bit = static_cast<uint8_t>((sym_bits >> (cfg.sf - 1u - row)) & 0x1u);
                inter[col * cfg.sf + row] = bit;
            }
        }

        for (uint32_t dst = 0; dst < inter_map.n_out; ++dst) {
            uint32_t src = inter_map.map[dst];
            deinter[dst] = (src < inter.size()) ? inter[src] : 0u;
        }

        for (uint32_t row = 0; row < cfg.sf; ++row) {
            uint16_t cw = 0u;
            for (uint32_t col = 0; col < cw_len; ++col) {
                cw = static_cast<uint16_t>((cw << 1) | deinter[row * cw_len + col]);
            }
            auto dec = lora::rx::gr::hamming_decode4(cw, static_cast<uint8_t>(cw_len), code_rate, tables);
            if (!dec) {
                DEBUGF("PAYLOAD FAIL: hamming decode error in block %zu row %u", block, row);
                return ret;
            }
            nibbles.push_back(static_cast<uint8_t>(dec->first & 0x0Fu));
        }
    }

    const size_t expected_bytes = static_cast<size_t>(hdr.payload_len_bytes) + (hdr.has_crc ? 2u : 0u);
    if (nibbles.empty()) {
        if (expected_bytes != 0u) {
            DEBUGF("PAYLOAD FAIL: decoded nibble stream empty");
            return ret;
        }
        ret.consumed_raw = payload_raw;
        ret.payload_data.clear();
        ret.ok = true;
        ret.crc_ok = !hdr.has_crc;
        return ret;
    }

    std::vector<uint8_t> bytes((nibbles.size() + 1u) / 2u, 0u);
    for (size_t i = 0; i < bytes.size(); ++i) {
        uint8_t low = nibbles[2u * i];
        uint8_t high = (2u * i + 1u < nibbles.size()) ? nibbles[2u * i + 1u] : 0u;
        bytes[i] = static_cast<uint8_t>((high << 4) | (low & 0x0Fu));
    }

    if (bytes.size() < expected_bytes) {
        DEBUGF("PAYLOAD FAIL: insufficient decoded bytes (have %zu, need %zu)", bytes.size(), expected_bytes);
        return ret;
    }

    bytes.resize(expected_bytes);
    lora::rx::gr::dewhiten_payload(std::span<uint8_t>(bytes.data(), bytes.size()));

    ret.payload_data.assign(bytes.begin(), bytes.begin() + hdr.payload_len_bytes);

    if (hdr.has_crc) {
        if (bytes.size() < ret.payload_data.size() + 2u) {
            DEBUGF("PAYLOAD FAIL: missing CRC bytes");
            return ret;
        }
        uint16_t crc_rx = static_cast<uint16_t>(bytes[ret.payload_data.size()]) |
                          static_cast<uint16_t>(bytes[ret.payload_data.size() + 1] << 8);
        lora::rx::gr::Crc16Ccitt crc;
        uint16_t crc_calc = crc.compute(ret.payload_data.data(), ret.payload_data.size());
        ret.crc_ok = (crc_calc == crc_rx);
    } else {
        ret.crc_ok = true;
    }

    ret.ok = true;
    ret.consumed_raw = payload_raw;

    return ret;
}

// Offline pipeline runner - fed in chunks (also offline)
std::vector<FrameCtx> run_pipeline_offline(const cfloat* iq, size_t iq_len, const RxConfig& cfg) {
    std::vector<FrameCtx> frames;

    if (!iq || iq_len == 0) {
        DEBUGF("Invalid input: iq=%p, iq_len=%zu", iq, iq_len);
        return frames;
    }

    // Performance measurement
    auto start_time = std::chrono::high_resolution_clock::now();

    Ring ring;
    Scheduler sch;

    try {
        sch.init(cfg);
    } catch (const std::exception& e) {
        DEBUGF("Failed to initialize scheduler: %s", e.what());
        return frames;
    }

    // Feed in small chunks (not required, but simulates streaming and gives history advantage)
    constexpr size_t CHUNK = 64 * 1024;
    size_t pos = 0;
    int frame_count = 0;
    int preamble_detections = 0;
    int header_decodes = 0;
    int payload_decodes = 0;

    while (pos < iq_len) {
        size_t push = std::min(CHUNK, iq_len - pos);

        // Check if we can write this chunk
        if (!ring.can_write(push)) {
            DEBUGF("Ring buffer full, cannot write %zu samples (capacity %zu, current tail %zu)",
                   push, ring.capacity(), ring.tail);
            break; // or handle differently
        }

        // Direct write without allocations
        cfloat* wptr = ring.wptr(ring.tail);
        if (!wptr) {
            DEBUGF("Invalid write pointer at tail %zu", ring.tail);
            break;
        }

        try {
            std::memcpy(wptr, iq + pos, push * sizeof(cfloat));
            ring.tail += push;
            pos += push;

            // Run scheduler as long as there are enough samples in window
            while (sch.step(ring)) {
                frame_count++;
                // Count different types of operations (simplified)
                preamble_detections++;
                header_decodes++;
                payload_decodes++;
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

    // At file end - run until done
    try {
        while (sch.step(ring)) {
            frame_count++;
            preamble_detections++;
            header_decodes++;
            payload_decodes++;
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

    // Performance metrics
    double samples_per_second = (double)iq_len / (duration.count() / 1e6);
    double frames_per_second = (double)frame_count / (duration.count() / 1e6);

    DEBUGF("Pipeline completed: processed %zu/%zu samples, found %d frames",
           pos, iq_len, frame_count);
    DEBUGF("Performance: %.2f MSamples/sec, %.2f frames/sec, %ld μs total",
           samples_per_second / 1e6, frames_per_second, duration.count());
    DEBUGF("Operations: %d preamble detections, %d header decodes, %d payload decodes",
           preamble_detections, header_decodes, payload_decodes);

    return frames;
}
