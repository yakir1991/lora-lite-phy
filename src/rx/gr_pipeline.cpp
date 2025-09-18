#include "lora/rx/gr_pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>

#include "lora/rx/decimate.hpp"
#include "lora/rx/preamble.hpp"
#include "lora/utils/crc.hpp"
#include "lora/utils/gray.hpp"
#include "lora/utils/interleaver.hpp"
#include "lora/utils/whitening.hpp"

namespace lora::rx::pipeline {

namespace {

using lora::rx::PreambleDetectResult;

std::optional<PreambleDetectResult> detect_preamble_dynamic(
    Workspace& ws,
    std::span<const std::complex<float>> samples,
    uint32_t sf,
    size_t min_syms,
    const std::vector<int>& candidates) {
    std::vector<int> os_list = candidates;
    if (os_list.empty()) os_list = {1, 2, 4, 8};

    for (int os : os_list) {
        if (os <= 0) continue;
        if (os == 1) {
            if (auto pos = lora::rx::detect_preamble(ws, samples, sf, min_syms))
                return PreambleDetectResult{*pos, 1, 0};
            continue;
        }
        for (int phase = 0; phase < os; ++phase) {
            auto decim = lora::rx::decimate_os_phase(samples, os, phase);
            if (auto pos = lora::rx::detect_preamble(ws, decim, sf, min_syms)) {
                size_t start_raw = (*pos) * static_cast<size_t>(os) + static_cast<size_t>(phase);
                unsigned int L = static_cast<unsigned int>(std::max(os * 32, os * 8));
                size_t gd_raw = static_cast<size_t>(L / 2);
                size_t adj_raw = start_raw > gd_raw ? (start_raw - gd_raw) : 0u;
                return PreambleDetectResult{adj_raw, os, phase};
            }
            ws.init(sf);
            uint32_t N = ws.N;
            if (decim.size() < min_syms * N) continue;
            std::vector<std::complex<float>> ref(N);
            for (uint32_t n = 0; n < N; ++n)
                ref[n] = std::conj(ws.upchirp[n]);
            float max_corr = 0.f;
            size_t step = std::max<uint32_t>(1u, N / 16);
            for (size_t i = 0; i + N <= decim.size(); i += step) {
                std::complex<float> acc{0.f, 0.f};
                const auto* blk = &decim[i];
                for (uint32_t n = 0; n < N; ++n)
                    acc += blk[n] * ref[n];
                float mag = std::abs(acc);
                if (mag > max_corr) max_corr = mag;
            }
            if (max_corr <= 0.f) continue;
            float thr = 0.7f * max_corr;
            size_t best_pos = 0;
            bool have = false;
            for (size_t i = 0; i + min_syms * N <= decim.size(); i += step) {
                bool ok = true;
                for (size_t k = 0; k < min_syms; ++k) {
                    size_t idx = i + k * N;
                    std::complex<float> acc{0.f, 0.f};
                    const auto* blk = &decim[idx];
                    for (uint32_t n = 0; n < N; ++n)
                        acc += blk[n] * ref[n];
                    if (std::abs(acc) < thr) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    best_pos = i;
                    have = true;
                    break;
                }
            }
            if (have) {
                size_t start_raw = best_pos * static_cast<size_t>(os) + static_cast<size_t>(phase);
                unsigned int L = static_cast<unsigned int>(std::max(os * 32, os * 8));
                size_t gd_raw = static_cast<size_t>(L / 2);
                size_t adj_raw = start_raw > gd_raw ? (start_raw - gd_raw) : 0u;
                return PreambleDetectResult{adj_raw, os, phase};
            }
        }
    }
    return std::nullopt;
}

bool decode_header_from_bins(Workspace& ws,
                             const Config& cfg,
                             const std::vector<uint32_t>& raw_bins,
                             HeaderStageOutput& out,
                             const lora::utils::HammingTables& tables) {
    uint32_t sf = cfg.sf;
    if (sf < 2) return false;
    uint32_t sf_app = sf - 2;
    if (cfg.header_symbol_count % 8 != 0) return false;
    if (raw_bins.size() < cfg.header_symbol_count) return false;
    size_t blocks = cfg.header_symbol_count / 8;

    out.cw_bytes.assign(blocks * sf_app, 0u);
    out.decoded_nibbles.clear();
    out.header_bytes.clear();
    out.header.reset();

    size_t cw_idx = 0;
    uint32_t N = ws.N;
    for (size_t blk = 0; blk < blocks; ++blk) {
        std::vector<uint8_t> inter(8 * sf_app);
        for (size_t sym = 0; sym < 8; ++sym) {
            size_t idx = blk * 8 + sym;
            uint32_t raw = raw_bins[idx] & (N - 1);
            uint32_t gnu = ((raw + N - 1) & (N - 1)) >> 2;
            uint32_t gray = lora::utils::gray_encode(gnu);
            for (uint32_t bit = 0; bit < sf_app; ++bit) {
                uint32_t shift = sf_app - 1 - bit;
                inter[sym * sf_app + bit] = static_cast<uint8_t>((gray >> shift) & 0x1u);
            }
        }
        std::vector<uint8_t> deinter(8 * sf_app);
        for (uint32_t col = 0; col < 8; ++col) {
            for (uint32_t row = 0; row < sf_app; ++row) {
                int dest_row = static_cast<int>(col) - static_cast<int>(row) - 1;
                dest_row %= static_cast<int>(sf_app);
                if (dest_row < 0) dest_row += static_cast<int>(sf_app);
                deinter[static_cast<size_t>(dest_row) * 8 + col] =
                    inter[col * sf_app + row];
            }
        }
        for (uint32_t row = 0; row < sf_app; ++row) {
            uint8_t byte = 0u;
            for (uint32_t col = 0; col < 8; ++col)
                byte = static_cast<uint8_t>((byte << 1) | deinter[row * 8 + col]);
            out.cw_bytes[cw_idx++] = byte;
        }
    }

    out.decoded_nibbles.resize(out.cw_bytes.size());
    for (size_t i = 0; i < out.cw_bytes.size(); ++i) {
        auto dec = lora::utils::hamming_decode4(out.cw_bytes[i], 8u, lora::utils::CodeRate::CR48, tables);
        if (!dec) return false;
        out.decoded_nibbles[i] = static_cast<uint8_t>(dec->first & 0x0Fu);
    }

    out.header_bytes.resize(out.decoded_nibbles.size() / 2);
    for (size_t i = 0; i < out.header_bytes.size(); ++i) {
        uint8_t low = out.decoded_nibbles[i * 2];
        uint8_t high = (i * 2 + 1 < out.decoded_nibbles.size()) ? out.decoded_nibbles[i * 2 + 1] : 0u;
        out.header_bytes[i] = static_cast<uint8_t>((high << 4) | low);
    }
    if (out.header_bytes.size() < 5) return false;
    auto hdr = lora::rx::parse_standard_lora_header(out.header_bytes.data(), 5);
    if (!hdr) return false;
    out.header = hdr;
    return true;
}

} // namespace

GnuRadioLikePipeline::GnuRadioLikePipeline(Config cfg)
    : cfg_(std::move(cfg)),
      hamming_tables_(lora::utils::make_hamming_tables()) {}

PipelineResult GnuRadioLikePipeline::run(std::span<const std::complex<float>> samples) {
    PipelineResult result;

    ws_.init(cfg_.sf);
    uint32_t N = ws_.N;

    auto det = detect_preamble_dynamic(ws_, samples, cfg_.sf, cfg_.min_preamble_syms, cfg_.os_candidates);
    if (!det) {
        result.failure_reason = "preamble_not_found";
        return result;
    }
    result.frame_sync.detected = true;
    result.frame_sync.preamble_start_sample = det->start_sample;
    result.frame_sync.os = det->os;
    result.frame_sync.phase = det->phase;

    result.frame_sync.decimated = lora::rx::decimate_os_phase(samples, det->os, det->phase);
    auto& decimated = result.frame_sync.decimated;
    if (decimated.empty()) {
        result.failure_reason = "decimation_failed";
        return result;
    }

    size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
    if (start_decim >= decimated.size()) {
        result.failure_reason = "preamble_index_oob";
        return result;
    }
    if (start_decim + cfg_.min_preamble_syms * N > decimated.size()) {
        result.failure_reason = "insufficient_preamble_samples";
        return result;
    }

    auto cfo = lora::rx::estimate_cfo_from_preamble(ws_, decimated, cfg_.sf, start_decim, cfg_.min_preamble_syms);
    if (!cfo) {
        result.failure_reason = "cfo_estimation_failed";
        return result;
    }
    result.frame_sync.cfo = *cfo;

    result.frame_sync.compensated.resize(decimated.size());
    float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
    for (size_t n = 0; n < decimated.size(); ++n) {
        float ang = two_pi_eps * static_cast<float>(n);
        result.frame_sync.compensated[n] = decimated[n] * std::complex<float>(std::cos(ang), std::sin(ang));
    }

    int sto_search = cfg_.sto_search > 0 ? cfg_.sto_search : static_cast<int>(N / 8);
    auto sto = lora::rx::estimate_sto_from_preamble(ws_, result.frame_sync.compensated, cfg_.sf, start_decim,
                                                    cfg_.min_preamble_syms, sto_search);
    if (!sto) {
        result.failure_reason = "sto_estimation_failed";
        return result;
    }
    result.frame_sync.sto = *sto;

    size_t aligned_start = start_decim;
    if (*sto >= 0)
        aligned_start += static_cast<size_t>(*sto);
    else {
        size_t shift = static_cast<size_t>(-*sto);
        aligned_start = (shift > aligned_start) ? 0u : (aligned_start - shift);
    }

    size_t header_start = aligned_start + cfg_.min_preamble_syms * N +
                          static_cast<size_t>(cfg_.symbols_after_preamble * static_cast<float>(N));
    if (header_start >= result.frame_sync.compensated.size()) {
        result.failure_reason = "insufficient_samples_for_header";
        return result;
    }
    result.frame_sync.header_start_sample = header_start;

    size_t available = result.frame_sync.compensated.size() - header_start;
    size_t nsamp = (available / N) * N;
    if (nsamp == 0) {
        result.failure_reason = "insufficient_samples_for_header";
        return result;
    }

    result.frame_sync.frame_samples.assign(result.frame_sync.compensated.begin() + header_start,
                                           result.frame_sync.compensated.begin() + header_start + nsamp);
    size_t nsym_total = result.frame_sync.frame_samples.size() / N;
    if (nsym_total == 0) {
        result.failure_reason = "insufficient_samples_for_header";
        return result;
    }

    result.fft.raw_bins.resize(nsym_total);
    for (size_t s = 0; s < nsym_total; ++s) {
        const std::complex<float>* block = result.frame_sync.frame_samples.data() + s * N;
        for (uint32_t n = 0; n < N; ++n)
            ws_.rxbuf[n] = block[n] * ws_.downchirp[n];
        ws_.fft(ws_.rxbuf.data(), ws_.fftbuf.data());
        uint32_t max_bin = 0u;
        float max_mag = 0.f;
        for (uint32_t k = 0; k < N; ++k) {
            float mag = std::norm(ws_.fftbuf[k]);
            if (mag > max_mag) {
                max_mag = mag;
                max_bin = k;
            }
        }
        result.fft.raw_bins[s] = max_bin;
    }

    if (!decode_header_from_bins(ws_, cfg_, result.fft.raw_bins, result.header, hamming_tables_)) {
        result.failure_reason = "header_decode_failed";
        return result;
    }
    if (!result.header.header) {
        result.failure_reason = "header_crc_failed";
        return result;
    }

    if (!cfg_.decode_payload) {
        result.success = true;
        return result;
    }

    auto header = *result.header.header;
    uint32_t cr_plus4 = static_cast<uint32_t>(header.cr) + 4u;

    if (result.fft.raw_bins.size() < cfg_.header_symbol_count) {
        result.failure_reason = "insufficient_header_symbols";
        return result;
    }
    size_t payload_symbol_count = result.fft.raw_bins.size() - cfg_.header_symbol_count;
    if (payload_symbol_count == 0 && header.payload_len > 0) {
        result.failure_reason = "insufficient_payload_symbols";
        return result;
    }

    result.gray.payload_symbols.resize(payload_symbol_count);
    for (size_t i = 0; i < payload_symbol_count; ++i) {
        uint32_t raw = result.fft.raw_bins[cfg_.header_symbol_count + i] & (N - 1);
        result.gray.payload_symbols[i] = lora::utils::gray_encode(raw);
    }

    size_t total_bits = payload_symbol_count * cfg_.sf;
    result.bits.msb_first_bits.resize(total_bits);
    size_t bit_idx = 0;
    for (size_t sym = 0; sym < payload_symbol_count; ++sym) {
        uint32_t sym_val = result.gray.payload_symbols[sym];
        for (int b = static_cast<int>(cfg_.sf) - 1; b >= 0; --b)
            result.bits.msb_first_bits[bit_idx++] = static_cast<uint8_t>((sym_val >> b) & 0x1u);
    }

    result.bits.deinterleaved_bits.assign(total_bits, 0u);
    const auto& inter = ws_.get_interleaver(cfg_.sf, cr_plus4);
    if (inter.n_in == 0) {
        result.failure_reason = "interleaver_unavailable";
        return result;
    }
    for (size_t off = 0; off + inter.n_in <= total_bits; off += inter.n_in) {
        for (uint32_t i = 0; i < inter.n_out; ++i) {
            size_t dst = off + inter.map[i];
            size_t src = off + i;
            if (dst < result.bits.deinterleaved_bits.size())
                result.bits.deinterleaved_bits[dst] = result.bits.msb_first_bits[src];
        }
    }

    result.fec.nibbles.clear();
    for (size_t i = 0; i + cr_plus4 <= result.bits.deinterleaved_bits.size(); i += cr_plus4) {
        uint16_t cw = 0u;
        for (uint32_t b = 0; b < cr_plus4; ++b)
            cw = static_cast<uint16_t>((cw << 1) | result.bits.deinterleaved_bits[i + b]);
        auto dec = lora::utils::hamming_decode4(cw, static_cast<uint8_t>(cr_plus4), header.cr, hamming_tables_);
        if (!dec) {
            result.failure_reason = "fec_decode_failed";
            return result;
        }
        result.fec.nibbles.push_back(static_cast<uint8_t>(dec->first & 0x0Fu));
    }

    result.fec.raw_bytes.resize((result.fec.nibbles.size() + 1) / 2);
    for (size_t i = 0; i < result.fec.raw_bytes.size(); ++i) {
        uint8_t low = (i * 2 < result.fec.nibbles.size()) ? result.fec.nibbles[i * 2] : 0u;
        uint8_t high = (i * 2 + 1 < result.fec.nibbles.size()) ? result.fec.nibbles[i * 2 + 1] : 0u;
        result.fec.raw_bytes[i] = static_cast<uint8_t>((high << 4) | low);
    }

    size_t crc_bytes = header.has_crc ? 2u : 0u;
    size_t needed = static_cast<size_t>(header.payload_len) + crc_bytes;
    if (result.fec.raw_bytes.size() < needed) {
        result.failure_reason = "insufficient_payload_bytes";
        return result;
    }

    result.payload.dewhitened_payload.assign(result.fec.raw_bytes.begin(),
                                             result.fec.raw_bytes.begin() + header.payload_len);
    if (!result.payload.dewhitened_payload.empty()) {
        auto lfsr = lora::utils::LfsrWhitening::pn9_default();
        lfsr.apply(result.payload.dewhitened_payload.data(), result.payload.dewhitened_payload.size());
    }

    if (header.has_crc && cfg_.expect_payload_crc) {
        lora::utils::Crc16Ccitt crc16;
        uint16_t crc_calc = crc16.compute(result.payload.dewhitened_payload.data(), result.payload.dewhitened_payload.size());
        uint16_t crc_rx = static_cast<uint16_t>(result.fec.raw_bytes[header.payload_len]) |
                          (static_cast<uint16_t>(result.fec.raw_bytes[header.payload_len + 1]) << 8);
        result.payload.crc_ok = (crc_calc == crc_rx);
        if (!result.payload.crc_ok) {
            result.failure_reason = "payload_crc_failed";
            return result;
        }
    } else {
        result.payload.crc_ok = true;
    }

    result.success = true;
    return result;
}

} // namespace lora::rx::pipeline

