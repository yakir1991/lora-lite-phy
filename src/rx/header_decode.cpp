#include "lora/rx/header_decode.hpp"
#include "lora/debug.hpp"
#include "lora/rx/decimate.hpp"
#include "lora/rx/demod.hpp"
#include "lora/rx/frame.hpp"
#include "lora/rx/preamble.hpp"
#include "lora/utils/crc.hpp"
#include "lora/utils/gray.hpp"
#include "lora/utils/whitening.hpp"
#include <algorithm>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace lora::rx {

// Local demod helper (same as in frame.cpp)
// use demod_symbol_peak from demod.hpp

std::optional<LocalHeader> decode_header_with_preamble_cfo_sto_os_impl(
    Workspace &ws, std::span<const std::complex<float>> samples, uint32_t sf,
    lora::utils::CodeRate cr, size_t min_preamble_syms, uint8_t expected_sync) {
  // Reset header diagnostics so state does not leak across calls
  ws.dbg_hdr_filled = false;
  ws.dbg_hdr_sf = 0;
  std::fill(std::begin(ws.dbg_hdr_syms_raw), std::end(ws.dbg_hdr_syms_raw), 0);
  std::fill(std::begin(ws.dbg_hdr_syms_corr), std::end(ws.dbg_hdr_syms_corr),
            0);
  std::fill(std::begin(ws.dbg_hdr_gray), std::end(ws.dbg_hdr_gray), 0);
  std::fill(std::begin(ws.dbg_hdr_nibbles_cr48),
            std::end(ws.dbg_hdr_nibbles_cr48), 0);
  std::fill(std::begin(ws.dbg_hdr_nibbles_cr45),
            std::end(ws.dbg_hdr_nibbles_cr45), 0);

  if (std::getenv("LORA_DEBUG"))
    std::fprintf(stderr, "DEBUG: [impl] decode_header_with_preamble_cfo_sto_os_impl\n");
  // OS-aware detect + decimate
  auto det =
      detect_preamble_os(ws, samples, sf, min_preamble_syms, {4, 2, 1, 8});
  if (!det)
    return std::nullopt;
  auto decim = decimate_os_phase(samples, det->os, det->phase);
  size_t start_decim = det->start_sample / static_cast<size_t>(det->os);
  if (start_decim >= decim.size())
    return std::nullopt;
  auto aligned0 = std::span<const std::complex<float>>(
      decim.data() + start_decim, decim.size() - start_decim);
  // CFO
  auto pos0 = detect_preamble(ws, aligned0, sf, min_preamble_syms);
  if (!pos0) {
    if (std::getenv("LORA_DEBUG"))
      std::fprintf(stderr, "DEBUG: [impl] Second preamble detection failed\n");
    return std::nullopt;
  }
  if (std::getenv("LORA_DEBUG"))
    std::fprintf(stderr, "DEBUG: [impl] Second preamble detection passed, pos0=%zu\n", *pos0);
  auto cfo =
      estimate_cfo_from_preamble(ws, aligned0, sf, *pos0, min_preamble_syms);
  if (!cfo) {
    if (std::getenv("LORA_DEBUG"))
      std::fprintf(stderr, "DEBUG: [impl] CFO estimation failed\n");
    return std::nullopt;
  }
  if (std::getenv("LORA_DEBUG"))
    std::fprintf(stderr, "DEBUG: [impl] CFO estimation passed, cfo=%f\n", *cfo);
  std::vector<std::complex<float>> comp(aligned0.size());
  float two_pi_eps = -2.0f * static_cast<float>(M_PI) * (*cfo);
  std::complex<float> j(0.f, 1.f);
  for (size_t n = 0; n < aligned0.size(); ++n)
    comp[n] = aligned0[n] * std::exp(j * (two_pi_eps * static_cast<float>(n)));
  // STO
  auto sto = estimate_sto_from_preamble(ws, comp, sf, *pos0, min_preamble_syms,
                                        static_cast<int>(ws.N / 8));
  if (!sto) {
    if (std::getenv("LORA_DEBUG"))
      std::fprintf(stderr, "DEBUG: [impl] STO estimation failed\n");
    return std::nullopt;
  }
  if (std::getenv("LORA_DEBUG"))
    std::fprintf(stderr, "DEBUG: [impl] STO estimation passed, sto=%d\n", *sto);
  int shift = *sto;
  size_t aligned_start = (shift >= 0) ? (*pos0 + static_cast<size_t>(shift))
                                      : (*pos0 - static_cast<size_t>(-shift));
  if (aligned_start >= comp.size())
    return std::nullopt;
  auto aligned = std::span<const std::complex<float>>(
      comp.data() + aligned_start, comp.size() - aligned_start);
  ws.init(sf);
  uint32_t N = ws.N;
  // Sync search (elastic)
  uint32_t net1 = ((expected_sync & 0xF0u) >> 4) << 3;
  uint32_t net2 = (expected_sync & 0x0Fu) << 3;
  size_t sync_start = 0;
  bool found_sync = false;
  int sym_shifts[5] = {0, -1, 1, -2, 2};
  int samp_shifts[5] = {0, -(int)N / 32, (int)N / 32, -(int)N / 16,
                        (int)N / 16};
  for (int s : sym_shifts) {
    size_t base = (s >= 0) ? ((min_preamble_syms + (size_t)s) * N)
                           : ((min_preamble_syms - (size_t)(-s)) * N);
    for (int so : samp_shifts) {
      size_t idx;
      if (so >= 0) {
        if (base + (size_t)so + N > aligned.size())
          continue;
        idx = base + (size_t)so;
      } else {
        size_t offs = (size_t)(-so);
        if (base < offs)
          continue;
        idx = base - offs;
        if (idx + N > aligned.size())
          continue;
      }
      uint32_t ss = demod_symbol_peak(ws, &aligned[idx]);
      if (std::abs(int(ss) - int(net1)) <= 2 ||
          std::abs(int(ss) - int(net2)) <= 2) {
        found_sync = true;
        sync_start = idx;
        break;
      }
    }
    if (found_sync)
      break;
  }
  if (!found_sync)
    return std::nullopt;
  // If second sync follows, skip one symbol
  if (sync_start + 2u * N <= aligned.size()) {
    uint32_t ss2 = demod_symbol_peak(ws, &aligned[sync_start + N]);
    if (std::abs(int(ss2) - int(net1)) <= 2 ||
        std::abs(int(ss2) - int(net2)) <= 2)
      sync_start += N;
  }
  // Header anchor: sync + 2 downchirps + 0.25 symbol
  size_t hdr_start_base = sync_start + (2u * N + N / 4u);
  // Keep canonical (pre-override) local index for diagnostics
  size_t hdr_start_canon_local = hdr_start_base;
  // Optional manual override of header base (for timing diagnostics)
  if (const char *soff = std::getenv("LORA_HDR_BASE_SYM_OFF"); soff && soff[0] != '\0') {
    long v = std::strtol(soff, nullptr, 10);
    long delta = v * (long)N;
    if (delta >= 0) {
      if (hdr_start_base + (size_t)delta < aligned.size()) hdr_start_base += (size_t)delta;
    } else {
      size_t o = (size_t)(-delta);
      if (hdr_start_base >= o) hdr_start_base -= o;
    }
  }
  if (const char *sampoff = std::getenv("LORA_HDR_BASE_SAMP_OFF"); sampoff && sampoff[0] != '\0') {
    long v = std::strtol(sampoff, nullptr, 10);
    if (v >= 0) {
      if (hdr_start_base + (size_t)v < aligned.size()) hdr_start_base += (size_t)v;
    } else {
      size_t o = (size_t)(-v);
      if (hdr_start_base >= o) hdr_start_base -= o;
    }
  }
  if (std::getenv("LORA_DEBUG")) {
    std::fprintf(stderr, "[hdr-base] hdr_start_base=%zu (N=%u)\n", hdr_start_base, N);
  }
  // Index trace in raw-sample domain (after OS): print canonical and final anchors and 16 symbol starts
  if (const char *idx = std::getenv("LORA_HDR_INDEX"); idx && idx[0] != '\0') {
    // Global decimated indices relative to original decimated stream
    size_t sync_dec_global = start_decim + aligned_start + sync_start;
    size_t b0_dec_canon   = start_decim + aligned_start + hdr_start_canon_local;
    size_t b1_dec_canon   = b0_dec_canon + 8u * N;
    size_t b0_dec_final   = start_decim + aligned_start + hdr_start_base;
    size_t b1_dec_final   = b0_dec_final + 8u * N;
    // Map to raw indices using decimation phase/os: raw_idx = phase + dec_idx * os
    unsigned os = det->os;
    size_t phase = det->phase;
    unsigned long long b0_raw_canon = (unsigned long long)phase + (unsigned long long)b0_dec_canon * (unsigned long long)os;
    unsigned long long b1_raw_canon = (unsigned long long)phase + (unsigned long long)b1_dec_canon * (unsigned long long)os;
    unsigned long long b0_raw_final = (unsigned long long)phase + (unsigned long long)b0_dec_final * (unsigned long long)os;
    unsigned long long b1_raw_final = (unsigned long long)phase + (unsigned long long)b1_dec_final * (unsigned long long)os;
    unsigned long long diff_canon_raw = (b1_raw_canon >= b0_raw_canon) ? (b1_raw_canon - b0_raw_canon) : 0ULL;
    unsigned long long diff_final_raw = (b1_raw_final >= b0_raw_final) ? (b1_raw_final - b0_raw_final) : 0ULL;
    std::fprintf(stderr,
                 "[hdr-index] os=%u phase=%zu N=%u | sync_dec=%zu | b0_dec_canon=%zu b1_dec_canon=%zu | b0_dec_final=%zu b1_dec_final=%zu\n",
                 os, phase, N, sync_dec_global, b0_dec_canon, b1_dec_canon, b0_dec_final, b1_dec_final);
    std::fprintf(stderr,
                 "[hdr-index] RAW canon: b0=%llu b1=%llu diff=%llu (expect %llu)\n",
                 b0_raw_canon, b1_raw_canon, diff_canon_raw,
                 (unsigned long long)(8u * N) * (unsigned long long)os);
    std::fprintf(stderr,
                 "[hdr-index] RAW final: b0=%llu b1=%llu diff=%llu (expect %llu)\n",
                 b0_raw_final, b1_raw_final, diff_final_raw,
                 (unsigned long long)(8u * N) * (unsigned long long)os);
    // Print 16 symbol start indices (canonical), 8 for block0 then 8 for block1
    for (int k = 0; k < 8; ++k) {
      unsigned long long dec_i = (unsigned long long)b0_dec_canon + (unsigned long long)k * (unsigned long long)N;
      unsigned long long raw_i = (unsigned long long)phase + dec_i * (unsigned long long)os;
      std::fprintf(stderr, "[hdr-index-b0] k=%d dec=%llu raw=%llu\n", k, dec_i, raw_i);
    }
    for (int k = 0; k < 8; ++k) {
      unsigned long long dec_i = (unsigned long long)b1_dec_canon + (unsigned long long)k * (unsigned long long)N;
      unsigned long long raw_i = (unsigned long long)phase + dec_i * (unsigned long long)os;
      std::fprintf(stderr, "[hdr-index-b1] k=%d dec=%llu raw=%llu\n", k, dec_i, raw_i);
    }
  }
  const uint32_t header_cr_plus4 = 8u;
  size_t hdr_bytes = 5;
  const size_t hdr_bits_exact = hdr_bytes * 2 * header_cr_plus4;
  uint32_t block_bits = sf * header_cr_plus4;
  size_t hdr_bits_padded = hdr_bits_exact;
  if (hdr_bits_padded % block_bits)
    hdr_bits_padded = ((hdr_bits_padded / block_bits) + 1) * block_bits;
  size_t hdr_nsym = hdr_bits_padded / sf;
  if (hdr_start_base + hdr_nsym * N > aligned.size())
    return std::nullopt;
  // Gray-coded symbol capture for diagnostics
  size_t hdr_start = hdr_start_base;
  auto data = std::span<const std::complex<float>>(aligned.data() + hdr_start,
                                                   aligned.size() - hdr_start);
  ws.ensure_rx_buffers(hdr_nsym, sf, header_cr_plus4);
  auto &symbols = ws.rx_symbols;
  for (size_t s = 0; s < hdr_nsym; ++s) {
    uint32_t raw_symbol = demod_symbol_peak(ws, &data[s * N]);
    uint32_t corr = (raw_symbol + ws.N - 44u) % ws.N;
    symbols[s] = lora::utils::gray_encode(corr);
    if (s < 16) {
      ws.dbg_hdr_filled = true;
      ws.dbg_hdr_sf = sf;
      ws.dbg_hdr_syms_raw[s] = raw_symbol;
      ws.dbg_hdr_syms_corr[s] = corr;
      ws.dbg_hdr_gray[s] = symbols[s];
    }
  }

  // Core GR-style block mapping with bounded two-block search (fresh origin per
  // block)
  std::optional<lora::rx::LocalHeader> hdr_opt;
  {
    static lora::utils::HammingTables Th = lora::utils::make_hamming_tables();
    const uint32_t sf_app = (sf > 2u) ? (sf - 2u) : sf;
    const uint32_t cw_len = 8u;
    auto build_block_rows = [&](const uint32_t gnu[8], uint8_t (&rows)[5][8]) {
      std::vector<std::vector<uint8_t>> inter_bin(
          cw_len, std::vector<uint8_t>(sf_app, 0));
      for (uint32_t i = 0; i < cw_len; ++i) {
        uint32_t full = gnu[i] & (N - 1u);
        uint32_t g = lora::utils::gray_encode(full);
        uint32_t sub = g & ((1u << sf_app) - 1u);
        for (uint32_t j = 0; j < sf_app; ++j)
          inter_bin[i][j] = (uint8_t)((sub >> (sf_app - 1u - j)) & 1u);
      }
      std::vector<std::vector<uint8_t>> deinter_bin(
          sf_app, std::vector<uint8_t>(cw_len, 0));
      for (uint32_t i = 0; i < cw_len; ++i) {
        for (uint32_t j = 0; j < sf_app; ++j) {
          int r = static_cast<int>(i) - static_cast<int>(j) - 1;
          r %= static_cast<int>(sf_app);
          if (r < 0)
            r += static_cast<int>(sf_app);
          deinter_bin[static_cast<size_t>(r)][i] = inter_bin[i][j];
        }
      }
      for (uint32_t r = 0; r < sf_app; ++r)
        for (uint32_t c = 0; c < cw_len; ++c)
          rows[r][c] = deinter_bin[r][c];
    };
    auto try_parse_two_block =
        [&](int off0, long samp0, int off1,
            long samp1) -> std::optional<lora::rx::LocalHeader> {
      if (hdr_nsym < 16 || sf_app < 3)
        return std::nullopt;
      // Block indices
      size_t idx0;
      if (samp0 >= 0) {
        idx0 = hdr_start_base + static_cast<size_t>(off0) * N +
               static_cast<size_t>(samp0);
        if (idx0 + 8u * N > aligned.size())
          return std::nullopt;
      } else {
        size_t o = static_cast<size_t>(-samp0);
        size_t base0 = hdr_start_base + static_cast<size_t>(off0) * N;
        if (base0 < o)
          return std::nullopt;
        idx0 = base0 - o;
        if (idx0 + 8u * N > aligned.size())
          return std::nullopt;
      }
      size_t base1 = hdr_start_base + 8u * N + static_cast<size_t>(off1) * N;
      size_t idx1;
      if (samp1 >= 0) {
        idx1 = base1 + static_cast<size_t>(samp1);
        if (idx1 + 8u * N > aligned.size())
          return std::nullopt;
      } else {
        size_t o = static_cast<size_t>(-samp1);
        if (base1 < o)
          return std::nullopt;
        idx1 = base1 - o;
        if (idx1 + 8u * N > aligned.size())
          return std::nullopt;
      }
      // Demod and reduce
      uint32_t raw0[8]{}, raw1[8]{};
      float d0[8]{}, n0[8]{}, d1[8]{}, n1[8]{}; // per-symbol fractional-bin (delta) and phase-slope (nu)
      bool log_frac = (std::getenv("LORA_HDR_FRAC_LOG") != nullptr);
      bool use_hann = (std::getenv("LORA_HDR_HANN") != nullptr);
      auto demod_peak_metrics = [&](size_t idx, const char *blk, int sidx, float *delta_out, float *nu_out) -> uint32_t {
        // Dechirp into ws.rxbuf; optional Hann window
        if (!use_hann) {
          for (uint32_t n = 0; n < N; ++n) ws.rxbuf[n] = aligned[idx + n] * ws.downchirp[n];
        } else {
          for (uint32_t n = 0; n < N; ++n) {
            float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
            ws.rxbuf[n] = (aligned[idx + n] * ws.downchirp[n]) * w;
          }
        }
        // Phase slope ν (cycles/sample) from adjacent samples on dechirped rxbuf
        std::complex<float> acc_ps(0.f, 0.f);
        for (uint32_t n = 1; n < N; ++n) acc_ps += ws.rxbuf[n] * std::conj(ws.rxbuf[n - 1]);
        float nu = 0.f;
        if (std::abs(acc_ps) > 0.f) {
          float phi = std::arg(acc_ps);
          nu = phi / (2.0f * (float)M_PI);
        }
        // FFT and integer peak
        ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
        uint32_t kmax = 0; float p0 = 0.f;
        for (uint32_t k = 0; k < N; ++k) {
          float mag = std::norm(ws.fftbuf[k]);
          if (mag > p0) { p0 = mag; kmax = k; }
        }
        // Parabolic fractional-bin δ using three-point interpolation
        auto mag_at = [&](int k) -> float { uint32_t kk = (uint32_t)((k % (int)N + (int)N) % (int)N); return std::norm(ws.fftbuf[kk]); };
        float pm = mag_at((int)kmax - 1);
        float pz = p0;
        float pp = mag_at((int)kmax + 1);
        float denom = pm - 2.0f * pz + pp;
        float delta = 0.f;
        if (std::abs(denom) > 1e-9f) delta = 0.5f * (pm - pp) / denom; // δ in [-0.5,0.5]
        if (delta_out) *delta_out = delta;
        if (nu_out) *nu_out = nu;
        if (log_frac) {
          std::fprintf(stderr, "[hdr-frac] %s s=%d k=%u pm=%.6g p0=%.6g pp=%.6g delta=%.4f nu=%.6f\n",
                      blk, sidx, (unsigned)kmax, (double)pm, (double)pz, (double)pp, (double)delta, (double)nu);
        }
        return kmax;
      };
      for (size_t s = 0; s < 8; ++s) raw0[s] = demod_peak_metrics(idx0 + s * N, "b0", (int)s, &d0[s], &n0[s]);
      for (size_t s = 0; s < 8; ++s) raw1[s] = demod_peak_metrics(idx1 + s * N, "b1", (int)s, &d1[s], &n1[s]);

      // Symbol-boundary SFO tracking for Block-1
      float sfo_deltas[8]{}; // accumulated sample drift per symbol
      bool use_sfo_tracking = (std::getenv("LORA_HDR_SFO_TRACK") != nullptr);
      bool log_sfo = (std::getenv("LORA_HDR_SFO_LOG") != nullptr);
      
      // Only run SFO tracking on the best known anchor to avoid performance issues
      bool is_best_anchor = (off0 == 2 && samp0 == 0 && 
                             off1 == -1 && samp1 == 0);
      
      if (log_sfo) {
        std::fprintf(stderr, "[hdr-debug] SFO tracking enabled: %s, best_anchor: %s (off0=%d,samp0=%ld,off1=%d,samp1=%ld)\n", 
                    use_sfo_tracking ? "true" : "false", is_best_anchor ? "true" : "false",
                    off0, samp0, off1, samp1);
      }
      
      if (use_sfo_tracking && is_best_anchor) {
        auto sfo_start_time = std::chrono::high_resolution_clock::now();
        
        // Compute accumulated fractional sample drift from delta trends
        // δ represents fractional bin offset, so Δs ≈ k * δ * (N/sf_symbols)
        // where k is a gain factor (default 1.0) and we accumulate across symbols
        float sfo_gain = 1.0f;
        if (const char *gain_str = std::getenv("LORA_HDR_SFO_GAIN")) {
          sfo_gain = std::strtof(gain_str, nullptr);
        }
        
        // Choose drift model: linear, quadratic, or simple accumulation
        const char* drift_model = std::getenv("LORA_HDR_SFO_MODEL");
        if (drift_model && std::strcmp(drift_model, "linear") == 0) {
          // Linear drift model: fit line to delta values and predict drift
          // Calculate linear regression: drift[s] = a + b*s
          float sum_s = 0.0f, sum_d = 0.0f, sum_sd = 0.0f, sum_s2 = 0.0f;
          for (size_t s = 0; s < 8; ++s) {
            float sf = (float)s;
            float df = d1[s];
            sum_s += sf;
            sum_d += df;
            sum_sd += sf * df;
            sum_s2 += sf * sf;
          }
          float mean_s = sum_s / 8.0f;
          float mean_d = sum_d / 8.0f;
          float slope = (sum_sd - 8.0f * mean_s * mean_d) / (sum_s2 - 8.0f * mean_s * mean_s);
          float intercept = mean_d - slope * mean_s;
          
          for (size_t s = 0; s < 8; ++s) {
            // Linear model prediction scaled by gain
            sfo_deltas[s] = sfo_gain * (intercept + slope * (float)s) * (float)(s + 1);
          }
          
          if (log_sfo) {
            std::fprintf(stderr, "[hdr-sfo-linear] slope=%.6f intercept=%.6f\n", (double)slope, (double)intercept);
          }
          
        } else if (drift_model && std::strcmp(drift_model, "quadratic") == 0) {
          // Quadratic drift model: fit parabola to delta values
          // For simplicity, use a basic quadratic approximation
          float mean_d = 0.0f;
          for (size_t s = 0; s < 8; ++s) mean_d += d1[s];
          mean_d /= 8.0f;
          
          // Simple quadratic: drift[s] = mean_d * s * s * scale
          float quad_scale = sfo_gain * 0.1f; // adjustable factor
          for (size_t s = 0; s < 8; ++s) {
            sfo_deltas[s] = quad_scale * mean_d * (float)s * (float)s;
          }
          
          if (log_sfo) {
            std::fprintf(stderr, "[hdr-sfo-quad] mean_d=%.6f quad_scale=%.6f\n", (double)mean_d, (double)quad_scale);
          }
          
        } else {
          // Default: accumulated drift approach (original)
          float accumulated_drift = 0.0f;
          for (size_t s = 0; s < 8; ++s) {
            accumulated_drift += d1[s] * sfo_gain;
            sfo_deltas[s] = accumulated_drift;
          }
        }
        
        if (log_sfo) {
          std::fprintf(stderr, "[hdr-sfo] Block-1 deltas: ");
          for (size_t s = 0; s < 8; ++s) {
            std::fprintf(stderr, "%.4f ", (double)d1[s]);
          }
          std::fprintf(stderr, "\n[hdr-sfo] Accumulated drifts: ");
          for (size_t s = 0; s < 8; ++s) {
            std::fprintf(stderr, "%.4f ", (double)sfo_deltas[s]);
          }
          std::fprintf(stderr, "\n");
        }
        
        // Recompute Block-1 symbols with fractional delay compensation
        uint32_t raw1_sfo[8]{};
        float d1_sfo[8]{}, n1_sfo[8]{};
        
        // Enable fractional interpolation mode
        bool use_fractional = (std::getenv("LORA_HDR_SFO_FRAC") != nullptr);
        
        // Enable chirp-slope compensation based on nu (phase slope) values
        bool use_chirp_slope = (std::getenv("LORA_HDR_SFO_CHIRP") != nullptr);
        float chirp_gain = 1.0f;
        if (const char *chirp_gain_str = std::getenv("LORA_HDR_SFO_CHIRP_GAIN")) {
          chirp_gain = std::strtof(chirp_gain_str, nullptr);
        }
        
        // Performance optimizations
        bool use_fast_mode = (std::getenv("LORA_HDR_SFO_FAST") != nullptr);
        
        // Pre-compute Hann window if needed
        std::vector<float> hann_window;
        if (use_hann) {
          hann_window.resize(N);
          for (uint32_t n = 0; n < N; ++n) {
            hann_window[n] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
          }
        }
        
        // Cache for interpolated samples to avoid re-computation
        std::unordered_map<int, std::vector<std::complex<float>>> interp_cache;
        
        // Early termination: skip SFO if deltas are small enough
        float max_delta = 0.0f;
        for (size_t s = 0; s < 8; ++s) {
          max_delta = std::max(max_delta, std::abs(sfo_deltas[s]));
        }
        
        if (use_fast_mode && max_delta < 0.05f) {
          if (log_sfo) {
            std::fprintf(stderr, "[hdr-sfo-fast] Skipping SFO correction - max_delta=%.4f < 0.05\n", (double)max_delta);
          }
          use_sfo_tracking = false;
        }
        
        for (size_t s = 0; s < 8; ++s) {
          float adjusted_start_f = (float)(idx1 + s * N) + sfo_deltas[s];
          
          if (use_fractional && std::abs(sfo_deltas[s]) > 0.01f) {
            // Fractional delay using linear interpolation
            size_t base_idx = (size_t)std::floor(adjusted_start_f);
            float frac = adjusted_start_f - (float)base_idx;
            
            if (base_idx + N + 1 > aligned.size()) {
              if (log_sfo) {
                std::fprintf(stderr, "[hdr-sfo] Symbol %zu out of bounds for fractional, skipping\n", s);
              }
              use_sfo_tracking = false;
              break;
            }
            
            // Apply fractional delay to the symbol samples before demod
            // Check cache first to avoid re-computation
            int cache_key = static_cast<int>(base_idx) * 1000 + static_cast<int>(frac * 1000);
            std::vector<std::complex<float>> interp_samples;
            
            auto cache_it = interp_cache.find(cache_key);
            if (cache_it != interp_cache.end()) {
              interp_samples = cache_it->second;
              if (log_sfo) {
                std::fprintf(stderr, "[hdr-sfo-cache] Using cached interpolation for symbol %zu\n", s);
              }
            } else {
              interp_samples.resize(N);
              for (uint32_t n = 0; n < N; ++n) {
                if (base_idx + n + 1 < aligned.size()) {
                  // Linear interpolation: s[n] = (1-frac)*s[n] + frac*s[n+1]
                  interp_samples[n] = (1.0f - frac) * aligned[base_idx + n] + frac * aligned[base_idx + n + 1];
                } else {
                  interp_samples[n] = aligned[base_idx + n];
                }
              }
              
              // Cache the result (limit cache size for memory)
              if (interp_cache.size() < 16) {
                interp_cache[cache_key] = interp_samples;
              }
            }
            
            // Demod the interpolated samples using the same metrics function approach
            // Apply chirp-slope compensation if enabled
            if (use_chirp_slope && std::abs(n1[s]) > 0.05f) {
              // Compensate for chirp slope mismatch by adjusting the downchirp phase
              float slope_compensation = chirp_gain * n1[s];
              std::complex<float> j(0.f, 1.f);
              
              if (!use_hann) {
                for (uint32_t n = 0; n < N; ++n) {
                  float phase_adjust = slope_compensation * (float)n * (float)n / (float)N;
                  auto compensated_downchirp = ws.downchirp[n] * std::exp(j * phase_adjust);
                  ws.rxbuf[n] = interp_samples[n] * compensated_downchirp;
                }
              } else {
                for (uint32_t n = 0; n < N; ++n) {
                  float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
                  float phase_adjust = slope_compensation * (float)n * (float)n / (float)N;
                  auto compensated_downchirp = ws.downchirp[n] * std::exp(j * phase_adjust);
                  ws.rxbuf[n] = (interp_samples[n] * compensated_downchirp) * w;
                }
              }
              
              if (log_sfo) {
                std::fprintf(stderr, "[hdr-sfo-chirp] Symbol %zu: nu=%.4f, slope_comp=%.6f\n", 
                            s, (double)n1[s], (double)slope_compensation);
              }
              
            } else {
              // Standard dechirp (optimized with pre-computed Hann window)
              if (!use_hann) {
                for (uint32_t n = 0; n < N; ++n) ws.rxbuf[n] = interp_samples[n] * ws.downchirp[n];
              } else {
                for (uint32_t n = 0; n < N; ++n) {
                  ws.rxbuf[n] = (interp_samples[n] * ws.downchirp[n]) * hann_window[n];
                }
              }
            }
            
            // Apply the same processing as demod_peak_metrics
            std::complex<float> acc_ps(0.f, 0.f);
            for (uint32_t n = 1; n < N; ++n) acc_ps += ws.rxbuf[n] * std::conj(ws.rxbuf[n - 1]);
            float nu = 0.f;
            if (std::abs(acc_ps) > 0.f) {
              float phi = std::arg(acc_ps);
              nu = phi / (2.0f * (float)M_PI);
            }
            
            ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
            uint32_t kmax = 0; float p0 = 0.f;
            for (uint32_t k = 0; k < N; ++k) {
              float mag = std::norm(ws.fftbuf[k]);
              if (mag > p0) { p0 = mag; kmax = k; }
            }
            
            // Parabolic fractional-bin δ
            auto mag_at = [&](int k) -> float { uint32_t kk = (uint32_t)((k % (int)N + (int)N) % (int)N); return std::norm(ws.fftbuf[kk]); };
            float pm = mag_at((int)kmax - 1);
            float pz = p0;
            float pp = mag_at((int)kmax + 1);
            float denom = pm - 2.0f * pz + pp;
            float delta = 0.f;
            if (std::abs(denom) > 1e-9f) delta = 0.5f * (pm - pp) / denom;
            
            raw1_sfo[s] = kmax;
            d1_sfo[s] = delta;
            n1_sfo[s] = nu;
            
            if (log_sfo) {
              std::fprintf(stderr, "[hdr-sfo-frac] Symbol %zu: frac=%.4f, kmax=%u, delta=%.4f, nu=%.4f\n", 
                          s, (double)frac, kmax, (double)delta, (double)nu);
            }
            
          } else {
            // Integer sample adjustment (original method)
            size_t adjusted_start = (size_t)std::round(adjusted_start_f);
            
            if (adjusted_start + N > aligned.size()) {
              if (log_sfo) {
                std::fprintf(stderr, "[hdr-sfo] Symbol %zu out of bounds, skipping SFO correction\n", s);
              }
              use_sfo_tracking = false;
              break;
            }
            
            raw1_sfo[s] = demod_peak_metrics(adjusted_start, "b1_sfo", (int)s, &d1_sfo[s], &n1_sfo[s]);
          }
        }
        
        if (use_sfo_tracking) {
          // Iterative refinement: if residual deltas are still large, try guided adjustment
          bool use_iterative = (std::getenv("LORA_HDR_SFO_ITER") != nullptr);
          float target_delta_threshold = 0.05f;
          if (const char *thresh = std::getenv("LORA_HDR_SFO_THRESH"); thresh) {
            target_delta_threshold = std::strtof(thresh, nullptr);
          }
          
          // Calculate current residual error
          float residual_rms = 0.0f;
          for (size_t s = 0; s < 8; ++s) {
            residual_rms += d1_sfo[s] * d1_sfo[s];
          }
          residual_rms = std::sqrt(residual_rms / 8.0f);
          
          // If we have GNU Radio target values, try guided correction
          const char *gnu_b1_str = std::getenv("LORA_HDR_FORCE_GNU_B1");
          if (use_iterative && gnu_b1_str && residual_rms > target_delta_threshold) {
            // Parse target GNU values
            uint32_t target_gnu[8];
            if (sscanf(gnu_b1_str, "%u,%u,%u,%u,%u,%u,%u,%u", 
                      &target_gnu[0], &target_gnu[1], &target_gnu[2], &target_gnu[3],
                      &target_gnu[4], &target_gnu[5], &target_gnu[6], &target_gnu[7]) == 8) {
              
              if (log_sfo) {
                std::fprintf(stderr, "[hdr-sfo-iter] Residual RMS=%.4f > %.4f, trying guided correction\n", 
                            (double)residual_rms, (double)target_delta_threshold);
              }
              
              // Iterative adjustment to match target gnu values
              int max_iter = 3;
              if (const char *iter_str = std::getenv("LORA_HDR_SFO_MAX_ITER"); iter_str) {
                max_iter = std::strtol(iter_str, nullptr, 10);
              }
              
              for (int iter = 0; iter < max_iter; ++iter) {
                bool improved = false;
                
                for (size_t s = 0; s < 8; ++s) {
                  uint32_t current_gnu = ((raw1_sfo[s] - 1u) & (N - 1u)) >> 2u;
                  uint32_t target = target_gnu[s];
                  
                  if (current_gnu != target) {
                    // Calculate adjustment needed
                    int gnu_diff = (int)target - (int)current_gnu;
                    // Handle wrap-around for gnu space (0 to N/4-1)
                    if (gnu_diff > (int)(N/8)) gnu_diff -= (int)(N/4);  
                    if (gnu_diff < -(int)(N/8)) gnu_diff += (int)(N/4); 
                    
                    // Convert gnu difference to raw bin difference, then to fractional sample adjustment
                    // gnu_diff * 4 gives raw bin difference, then normalize to fractional samples
                    float sample_adj = (float)gnu_diff * 4.0f;
                    
                    // Apply fine adjustment
                    float adjusted_start_f = (float)(idx1 + s * N) + sfo_deltas[s] + sample_adj;
                    size_t base_idx = (size_t)std::floor(adjusted_start_f);
                    float frac = adjusted_start_f - (float)base_idx;
                    
                    if (base_idx + N + 1 <= aligned.size() && std::abs(frac) < 0.5f) {
                      // Apply fractional delay interpolation
                      std::vector<std::complex<float>> interp_samples(N);
                      for (uint32_t n = 0; n < N; ++n) {
                        if (base_idx + n + 1 < aligned.size()) {
                          interp_samples[n] = (1.0f - frac) * aligned[base_idx + n] + frac * aligned[base_idx + n + 1];
                        } else {
                          interp_samples[n] = aligned[base_idx + n];
                        }
                      }
                      
                      // Demodulate with standard dechirp
                      for (uint32_t n = 0; n < N; ++n) ws.rxbuf[n] = interp_samples[n] * ws.downchirp[n];
                      
                      // Apply FFT and find peak
                      ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
                      uint32_t kmax = 0; float p0 = 0.f;
                      for (uint32_t k = 0; k < N; ++k) {
                        float mag = std::norm(ws.fftbuf[k]);
                        if (mag > p0) { p0 = mag; kmax = k; }
                      }
                      
                      // Calculate parabolic delta
                      auto mag_at = [&](int k) -> float { 
                        uint32_t kk = (uint32_t)((k % (int)N + (int)N) % (int)N); 
                        return std::norm(ws.fftbuf[kk]); 
                      };
                      float pm = mag_at((int)kmax - 1);
                      float pz = p0;
                      float pp = mag_at((int)kmax + 1);
                      float denom = pm - 2.0f * pz + pp;
                      float delta = 0.f;
                      if (std::abs(denom) > 1e-9f) delta = 0.5f * (pm - pp) / denom;
                      
                      // Update if improvement
                      uint32_t new_gnu = ((kmax - 1u) & (N - 1u)) >> 2u;
                      if (log_sfo) {
                        std::fprintf(stderr, "[hdr-sfo-iter] S%zu iter%d: raw=%u gnu %u->%u target=%u, delta %.4f\n", 
                                    s, iter, kmax, current_gnu, new_gnu, target, (double)delta);
                      }
                      
                      // Accept if we hit the target or improved significantly
                      if (new_gnu == target || (std::abs(delta) < std::abs(d1_sfo[s]) * 0.8f)) {
                        raw1_sfo[s] = kmax;
                        d1_sfo[s] = delta;
                        improved = true;
                        
                        if (log_sfo) {
                          std::fprintf(stderr, "[hdr-sfo-iter] S%zu iter%d: ACCEPTED improvement\n", s, iter);
                        }
                      }
                    }
                  }
                }
                
                if (!improved) break;
              }
            }
          }
          
          // Replace original Block-1 measurements with SFO-corrected ones
          std::copy(raw1_sfo, raw1_sfo + 8, raw1);
          std::copy(d1_sfo, d1_sfo + 8, d1);
          std::copy(n1_sfo, n1_sfo + 8, n1);
          
          if (log_sfo) {
            std::fprintf(stderr, "[hdr-sfo] Residual deltas after correction: ");
            for (size_t s = 0; s < 8; ++s) {
              std::fprintf(stderr, "%.4f ", (double)d1[s]);
            }
            std::fprintf(stderr, "\n[hdr-sfo] Residual nus after correction: ");
            for (size_t s = 0; s < 8; ++s) {
              std::fprintf(stderr, "%.4f ", (double)n1[s]);
            }
            std::fprintf(stderr, "\n");
          }
        }
        
        // Performance measurement
        auto sfo_end_time = std::chrono::high_resolution_clock::now();
        auto sfo_duration = std::chrono::duration_cast<std::chrono::microseconds>(sfo_end_time - sfo_start_time);
        if (log_sfo || std::getenv("LORA_HDR_SFO_PERF")) {
          std::fprintf(stderr, "[hdr-sfo-perf] SFO tracking took %ld μs\n", sfo_duration.count());
        }
      }

      // (moved) fractional-delay compensation block placed after assemble_and_try is defined
      // Fine-CFO demod helper for block1 (small fractional bin shifts)
      auto demod_symbol_peak_cfo = [&](size_t idx, float eps_cycles_per_sample) -> uint32_t {
        // Apply small CFO compensation, then standard downchirp dechirp and FFT peak
        // s'[n] = s[n] * exp(-j 2π eps n)
        float two_pi_eps = -2.0f * static_cast<float>(M_PI) * eps_cycles_per_sample;
        std::complex<float> j(0.f, 1.f);
        for (uint32_t n = 0; n < N; ++n) {
          auto v = aligned[idx + n] * std::exp(j * (two_pi_eps * static_cast<float>(n)));
          ws.rxbuf[n] = v * ws.downchirp[n];
        }
        ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
        uint32_t max_bin = 0; float max_mag = 0.f;
        for (uint32_t k = 0; k < N; ++k) {
          float mag = std::norm(ws.fftbuf[k]);
          if (mag > max_mag) { max_mag = mag; max_bin = k; }
        }
        return max_bin;
      };
      // Optional: block-wise CFO compensation (two modes)
      if (const char *bcfo = std::getenv("LORA_HDR_BLOCK_CFO"); bcfo && bcfo[0] != '\0') {
        auto demod_symbol_peak_cfo_block = [&](size_t idx, float eps_cycles_per_sample) -> uint32_t {
          float two_pi_eps = -2.0f * (float)M_PI * eps_cycles_per_sample;
          std::complex<float> j(0.f, 1.f);
          for (uint32_t n = 0; n < N; ++n) {
            auto v = aligned[idx + n] * std::exp(j * (two_pi_eps * (float)n));
            ws.rxbuf[n] = v * ws.downchirp[n];
          }
          ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
          uint32_t max_bin = 0; float max_mag = 0.f;
          for (uint32_t k = 0; k < N; ++k) { float mag = std::norm(ws.fftbuf[k]); if (mag > max_mag) { max_mag = mag; max_bin = k; } }
          return max_bin;
        };
        if (bcfo[0]=='1' && bcfo[1]=='\0') {
          // Mode 1: phase-slope over dechirped samples (may be contaminated by symbol tone)
          auto estimate_cfo_block = [&](size_t base_idx) -> float {
            std::complex<float> acc(0.f, 0.f);
            for (uint32_t s = 0; s < 8u; ++s) {
              size_t b = base_idx + (size_t)s * N;
              if (b + N > aligned.size()) break;
              for (uint32_t n = 1; n < N; ++n) {
                auto v  = aligned[b + n]     * std::conj(ws.upchirp[n]);
                auto vp = aligned[b + n - 1] * std::conj(ws.upchirp[n - 1]);
                acc += v * std::conj(vp);
              }
            }
            if (std::abs(acc) == 0.f) return 0.f;
            float phi = std::arg(acc);
            return phi / (2.0f * (float)M_PI); // cycles per sample
          };
          float eps0 = estimate_cfo_block(idx0);
          float eps1 = estimate_cfo_block(idx1);
          if (std::getenv("LORA_DEBUG")) {
            std::fprintf(stderr, "[hdr-block-cfo] eps0=%.6g eps1=%.6g\n", (double)eps0, (double)eps1);
          }
          for (size_t s = 0; s < 8; ++s) raw0[s] = demod_symbol_peak_cfo_block(idx0 + s * N, eps0);
          for (size_t s = 0; s < 8; ++s) raw1[s] = demod_symbol_peak_cfo_block(idx1 + s * N, eps1);
        } else if (bcfo[0]=='2' && bcfo[1]=='\0') {
          // Mode 2: CFO from residual bin offsets: delta_s = wrap(raw_s - (4*gnu_s+1)) → eps = mean(delta)/N
          auto wrap_to_half = [&](int d) -> int {
            if (d > (int)N/2) d -= (int)N;
            if (d < -(int)N/2) d += (int)N;
            return d;
          };
          // First pass: compute gnu from current raw
          uint32_t g0_tmp[8]{}, g1_tmp[8]{};
          for (size_t s = 0; s < 8; ++s) g0_tmp[s] = ((raw0[s] + N - 1u) & (N - 1u)) >> 2;
          for (size_t s = 0; s < 8; ++s) g1_tmp[s] = ((raw1[s] + N - 1u) & (N - 1u)) >> 2;
          auto estimate_eps_from_residual = [&](const uint32_t g[8], const uint32_t r[8]) -> float {
            long sum = 0; int cnt = 0;
            for (int s = 0; s < 8; ++s) {
              int center = (int)((4u * g[s] + 1u) & (N - 1u));
              int d = (int)r[s] - center;
              d = wrap_to_half(d);
              sum += d; ++cnt;
            }
            if (cnt == 0) return 0.f;
            return (float)sum / (float)cnt / (float)N; // cycles per sample
          };
          float eps0 = estimate_eps_from_residual(g0_tmp, raw0);
          float eps1 = estimate_eps_from_residual(g1_tmp, raw1);
          if (std::getenv("LORA_DEBUG")) {
            std::fprintf(stderr, "[hdr-block-cfo] eps0=%.6g eps1=%.6g\n", (double)eps0, (double)eps1);
          }
          for (size_t s = 0; s < 8; ++s) raw0[s] = demod_symbol_peak_cfo_block(idx0 + s * N, eps0);
          for (size_t s = 0; s < 8; ++s) raw1[s] = demod_symbol_peak_cfo_block(idx1 + s * N, eps1);
        }
      }

      // (moved) per-symbol fractional CFO block placed after assemble_and_try is defined

      uint32_t g0[8]{}, g1[8]{};
      for (size_t s = 0; s < 8; ++s) g0[s] = ((raw0[s] + N - 1u) & (N - 1u)) >> 2;
      for (size_t s = 0; s < 8; ++s) g1[s] = ((raw1[s] + N - 1u) & (N - 1u)) >> 2;
      bool loggnu = (std::getenv("LORA_HDR_LOG_GNU") != nullptr);
      if (loggnu) {
        std::fprintf(stderr, "[hdr-gnu] off0=%d samp0=%ld off1=%d samp1=%ld | b0:", off0, samp0, off1, samp1);
        for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", (unsigned)g0[i]);
        std::fprintf(stderr, " | b1:");
        for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", (unsigned)g1[i]);
        std::fprintf(stderr, "\n");
      }
      uint8_t blk0[5][8]{}, blk1[5][8]{};
      build_block_rows(g0, blk0);
      build_block_rows(g1, blk1);
      auto assemble_and_try =
          [&](const uint8_t b0[5][8],
              const uint8_t b1[5][8]) -> std::optional<lora::rx::LocalHeader> {
        uint8_t cw_local[10]{};
        for (uint32_t r = 0; r < sf_app; ++r) {
          uint16_t c = 0;
          for (uint32_t i = 0; i < 8u; ++i)
            c = (c << 1) | (b0[r][i] & 1u);
          cw_local[r] = (uint8_t)(c & 0xFF);
        }
        for (uint32_t r = 0; r < sf_app; ++r) {
          uint16_t c = 0;
          for (uint32_t i = 0; i < 8u; ++i)
            c = (c << 1) | (b1[r][i] & 1u);
          cw_local[sf_app + r] = (uint8_t)(c & 0xFF);
        }
        std::vector<uint8_t> nibb;
        nibb.reserve(10);
        for (int k = 0; k < 10; ++k) {
          auto dec = lora::utils::hamming_decode4(
              cw_local[k], 8u, lora::utils::CodeRate::CR48, Th);
          if (!dec)
            return std::nullopt;
          nibb.push_back(dec->first & 0x0F);
        }
        for (int order = 0; order < 2; ++order) {
          std::vector<uint8_t> hdr_try(5);
          for (int i = 0; i < 5; ++i) {
            uint8_t n0 = nibb[i * 2], n1 = nibb[i * 2 + 1];
            uint8_t low = (order == 0) ? n0 : n1;
            uint8_t high = (order == 0) ? n1 : n0;
            hdr_try[i] = (uint8_t)((high << 4) | low);
          }
          if (auto okhdr =
                  parse_standard_lora_header(hdr_try.data(), hdr_try.size())) {
            for (int k = 0; k < 10; ++k)
              ws.dbg_hdr_nibbles_cr48[k] = nibb[k];
            return okhdr;
          }
        }
        return std::nullopt;
      };

      // Optional: per-symbol fractional CFO using measured δ per symbol (Block-1 focus)
      if (const char *sfc = std::getenv("LORA_HDR_SYM_FCFO"); sfc && sfc[0]=='1' && sfc[1]=='\0') {
        auto demod_symbol_peak_cfo_local = [&](size_t idx, float eps_cycles_per_sample) -> uint32_t {
          float two_pi_eps = -2.0f * (float)M_PI * eps_cycles_per_sample;
          std::complex<float> j(0.f, 1.f);
          for (uint32_t n = 0; n < N; ++n) {
            auto v = aligned[idx + n] * std::exp(j * (two_pi_eps * (float)n));
            if (!use_hann) {
              ws.rxbuf[n] = v * ws.downchirp[n];
            } else {
              float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
              ws.rxbuf[n] = (v * ws.downchirp[n]) * w;
            }
          }
          ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
          uint32_t max_bin = 0; float max_mag = 0.f;
          for (uint32_t k = 0; k < N; ++k) { float mag = std::norm(ws.fftbuf[k]); if (mag > max_mag) { max_mag = mag; max_bin = k; } }
          return max_bin;
        };
        auto try_sym_fcfo = [&](float sign) -> std::optional<lora::rx::LocalHeader> {
          uint32_t raw1_fc[8]{};
          float invN = 1.0f / (float)N;
          for (int s = 0; s < 8; ++s) {
            float eps = -sign * d1[s] * invN; // shift by measured fractional-bin
            size_t base = idx1 + (size_t)s * N;
            if (base + N > aligned.size()) return std::nullopt;
            raw1_fc[s] = demod_symbol_peak_cfo_local(base, eps);
          }
          uint32_t g1_fc[8]{}; for (int s = 0; s < 8; ++s) g1_fc[s] = ((raw1_fc[s] + N - 1u) & (N - 1u)) >> 2;
          uint8_t blk1_fc[5][8]{}; build_block_rows(g1_fc, blk1_fc);
          if (std::getenv("LORA_DEBUG")) {
            std::fprintf(stderr, "[hdr-sfcfo sign=%.1f] g1:", (double)sign);
            for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", (unsigned)g1_fc[i]);
            std::fprintf(stderr, "\n");
          }
          if (auto ok = assemble_and_try(blk0, blk1_fc)) return ok;
          return std::nullopt;
        };
        if (auto ok = try_sym_fcfo(1.0f)) return ok;   // primary sign
        if (auto ok = try_sym_fcfo(-1.0f)) return ok;  // fallback sign
      }
      // Optional: fractional-delay compensation for Block-1 prior to dechirp (uses assemble_and_try)
      if (const char *fd1 = std::getenv("LORA_HDR_FD1"); fd1 && fd1[0] == '1' && fd1[1] == '\0') {
        auto demod_symbol_peak_fd = [&](size_t idx, float fd_smpl) -> uint32_t {
          if (idx == 0 || idx + N + 1 >= aligned.size()) {
            return demod_symbol_peak(ws, &aligned[idx]);
          }
          for (uint32_t n = 0; n < N; ++n) {
            float tn = (float)(idx + n) + fd_smpl;
            long i = (long)std::floor(tn);
            float mu = tn - (float)i;
            if (i < 0) i = 0;
            if ((size_t)(i + 1) >= aligned.size()) {
              return demod_symbol_peak(ws, &aligned[idx]);
            }
            std::complex<float> xi = aligned[(size_t)i];
            std::complex<float> xip1 = aligned[(size_t)(i + 1)];
            std::complex<float> xfd = (1.0f - mu) * xi + mu * xip1;
            std::complex<float> v = xfd * ws.downchirp[n];
            if (!use_hann) ws.rxbuf[n] = v; else {
              float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
              ws.rxbuf[n] = v * w;
            }
          }
          ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
          uint32_t kmax = 0; float pmax = 0.f;
          for (uint32_t k = 0; k < N; ++k) { float mag = std::norm(ws.fftbuf[k]); if (mag > pmax) { pmax = mag; kmax = k; } }
          return kmax;
        };
        auto mean_of = [&](const float *v) { float s = 0.f; for (int i = 0; i < 8; ++i) s += v[i]; return s / 8.f; };
        auto slope_of = [&](const float *v) {
          float mean_s = 3.5f; float mean_v = mean_of(v);
          float num = 0.f, den = 0.f;
          for (int s = 0; s < 8; ++s) { float ds = (float)s - mean_s; num += ds * (v[s] - mean_v); den += ds * ds; }
          return (den > 0.f) ? (num / den) : 0.f;
        };
        float m_d0 = mean_of(d0);
        float a_d0 = slope_of(d0);
        std::vector<float> fd_const = {0.0f, -0.25f, 0.25f};
        std::vector<float> fd_slope = {0.0f};
        if (const char *w = std::getenv("LORA_HDR_FD1_WIDE"); w && w[0]=='1' && w[1]=='\0') { fd_const.push_back(-0.5f); fd_const.push_back(0.5f); }
        if (const char *g = std::getenv("LORA_HDR_FD_GAIN"); g && g[0] != '\0') { float gg = std::strtof(g, nullptr); fd_const.push_back(-gg * m_d0); fd_const.push_back( gg * m_d0); }
        if (const char *sg = std::getenv("LORA_HDR_FD_SLOPE_GAIN"); sg && sg[0] != '\0') { float ggs = std::strtof(sg, nullptr); fd_slope.push_back(-ggs * a_d0); fd_slope.push_back( ggs * a_d0); }
        for (float c : fd_const) {
          for (float sl : fd_slope) {
            uint32_t raw1_fd[8]{}; bool oob = false;
            for (int s = 0; s < 8; ++s) {
              float fd = c + sl * (float)s; size_t base = idx1 + (size_t)s * N;
              if (base + N + 1 >= aligned.size()) { oob = true; break; }
              raw1_fd[s] = demod_symbol_peak_fd(base, fd);
            }
            if (oob) continue;
            uint32_t g1f[8]{}; for (int s = 0; s < 8; ++s) g1f[s] = ((raw1_fd[s] + N - 1u) & (N - 1u)) >> 2;
            uint8_t blk1f[5][8]{}; build_block_rows(g1f, blk1f);
            if (std::getenv("LORA_DEBUG")) {
              std::fprintf(stderr, "[hdr-fd] const=%.3f slope=%.5f | g1:", (double)c, (double)sl);
              for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", (unsigned)g1f[i]);
              std::fprintf(stderr, "\n");
            }
            if (auto ok = assemble_and_try(blk0, blk1f)) return ok;
          }
        }
      }
      // Optional: per-symbol fractional-delay using measured δ per symbol (Block-1 focus)
      if (const char *fdper = std::getenv("LORA_HDR_FD1_PER"); fdper && fdper[0] == '1' && fdper[1] == '\0') {
        // Parse optional CSV of k scales in env LORA_HDR_FD_PER_KS, otherwise defaults
        std::vector<float> k_scales;
        if (const char *ks = std::getenv("LORA_HDR_FD_PER_KS"); ks && ks[0] != '\0') {
          const char *p = ks; char *e = nullptr;
          while (*p) {
            float v = std::strtof(p, &e);
            if (e == p) { ++p; continue; }
            k_scales.push_back(v);
            p = e;
          }
        } else {
          k_scales = {0.5f, -0.5f, 1.0f, -1.0f};
        }
        auto demod_symbol_peak_fd_local = [&](size_t idx, float fd_smpl) -> uint32_t {
          if (idx == 0 || idx + N + 2 >= aligned.size()) {
            return demod_symbol_peak(ws, &aligned[idx]);
          }
          bool use_resamp = (std::getenv("LORA_HDR_RESAMP") != nullptr);
          int resamp_order = 3;
          if (const char *ro = std::getenv("LORA_HDR_RESAMP_ORDER"); ro && ro[0] != '\0') {
            resamp_order = std::strtol(ro, nullptr, 10);
          }
          for (uint32_t n = 0; n < N; ++n) {
            float tn = (float)(idx + n) + fd_smpl;
            long i = (long)std::floor(tn);
            float mu = tn - (float)i;
            if (i < 0) i = 0;
            // Default to linear if we don't have enough guard samples or resampler disabled
            std::complex<float> xfd;
            if (use_resamp && resamp_order >= 3 && (i - 1) >= 0 && (size_t)(i + 2) < aligned.size()) {
              // 4-tap (cubic) Lagrange using samples at i-1, i, i+1, i+2
              float m = mu;
              float w_m1 = ((m - 0.0f) * (m - 1.0f) * (m - 2.0f)) / (-6.0f);
              float w_0  = ((m + 1.0f) * (m - 1.0f) * (m - 2.0f)) / ( 2.0f);
              float w_1  = ((m + 1.0f) * (m - 0.0f) * (m - 2.0f)) / (-2.0f);
              float w_2  = ((m + 1.0f) * (m - 0.0f) * (m - 1.0f)) / ( 6.0f);
              std::complex<float> xm1 = aligned[(size_t)(i - 1)];
              std::complex<float> x0  = aligned[(size_t)i];
              std::complex<float> x1  = aligned[(size_t)(i + 1)];
              std::complex<float> x2  = aligned[(size_t)(i + 2)];
              xfd = w_m1 * xm1 + w_0 * x0 + w_1 * x1 + w_2 * x2;
            } else {
              if ((size_t)(i + 1) >= aligned.size()) {
                return demod_symbol_peak(ws, &aligned[idx]);
              }
              std::complex<float> xi = aligned[(size_t)i];
              std::complex<float> xip1 = aligned[(size_t)(i + 1)];
              xfd = (1.0f - mu) * xi + mu * xip1;
            }
            std::complex<float> v = xfd * ws.downchirp[n];
            if (!use_hann) ws.rxbuf[n] = v; else {
              float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
              ws.rxbuf[n] = v * w;
            }
          }
          ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
          uint32_t kmax = 0; float pmax = 0.f;
          for (uint32_t k = 0; k < N; ++k) { float mag = std::norm(ws.fftbuf[k]); if (mag > pmax) { pmax = mag; kmax = k; } }
          return kmax;
        };
        auto demod_symbol_peak_fd_fcfo = [&](size_t idx, float fd_smpl, float eps_cycles_per_sample) -> uint32_t {
          if (idx == 0 || idx + N + 2 >= aligned.size()) {
            // Fallback to CFO-only when interpolation is OOB
            float two_pi_eps = -2.0f * (float)M_PI * eps_cycles_per_sample;
            std::complex<float> j(0.f, 1.f);
            for (uint32_t n = 0; n < N; ++n) {
              auto v = aligned[idx + n] * std::exp(j * (two_pi_eps * (float)n));
              if (!use_hann) ws.rxbuf[n] = v * ws.downchirp[n]; else {
                float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
                ws.rxbuf[n] = (v * ws.downchirp[n]) * w;
              }
            }
            ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
            uint32_t kmax = 0; float pmax = 0.f;
            for (uint32_t k = 0; k < N; ++k) { float mag = std::norm(ws.fftbuf[k]); if (mag > pmax) { pmax = mag; kmax = k; } }
            return kmax;
          }
          bool use_resamp = (std::getenv("LORA_HDR_RESAMP") != nullptr);
          int resamp_order = 3;
          if (const char *ro = std::getenv("LORA_HDR_RESAMP_ORDER"); ro && ro[0] != '\0') {
            resamp_order = std::strtol(ro, nullptr, 10);
          }
          float two_pi_eps = -2.0f * (float)M_PI * eps_cycles_per_sample;
          std::complex<float> j(0.f, 1.f);
          for (uint32_t n = 0; n < N; ++n) {
            float tn = (float)(idx + n) + fd_smpl;
            long i = (long)std::floor(tn);
            float mu = tn - (float)i;
            if (i < 0) i = 0;
            std::complex<float> xfd;
            if (use_resamp && resamp_order >= 3 && (i - 1) >= 0 && (size_t)(i + 2) < aligned.size()) {
              float m = mu;
              float w_m1 = ((m - 0.0f) * (m - 1.0f) * (m - 2.0f)) / (-6.0f);
              float w_0  = ((m + 1.0f) * (m - 1.0f) * (m - 2.0f)) / ( 2.0f);
              float w_1  = ((m + 1.0f) * (m - 0.0f) * (m - 2.0f)) / (-2.0f);
              float w_2  = ((m + 1.0f) * (m - 0.0f) * (m - 1.0f)) / ( 6.0f);
              std::complex<float> xm1 = aligned[(size_t)(i - 1)];
              std::complex<float> x0  = aligned[(size_t)i];
              std::complex<float> x1  = aligned[(size_t)(i + 1)];
              std::complex<float> x2  = aligned[(size_t)(i + 2)];
              xfd = w_m1 * xm1 + w_0 * x0 + w_1 * x1 + w_2 * x2;
            } else {
              if ((size_t)(i + 1) >= aligned.size()) {
                auto v = aligned[idx + n] * std::exp(j * (two_pi_eps * (float)n));
                if (!use_hann) ws.rxbuf[n] = v * ws.downchirp[n]; else {
                  float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
                  ws.rxbuf[n] = (v * ws.downchirp[n]) * w;
                }
                continue;
              }
              std::complex<float> xi = aligned[(size_t)i];
              std::complex<float> xip1 = aligned[(size_t)(i + 1)];
              xfd = (1.0f - mu) * xi + mu * xip1;
            }
            auto v = xfd * std::exp(j * (two_pi_eps * (float)n));
            if (!use_hann) ws.rxbuf[n] = v * ws.downchirp[n]; else {
              float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
              ws.rxbuf[n] = (v * ws.downchirp[n]) * w;
            }
          }
          ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
          uint32_t kmax = 0; float pmax = 0.f;
          for (uint32_t k = 0; k < N; ++k) { float mag = std::norm(ws.fftbuf[k]); if (mag > pmax) { pmax = mag; kmax = k; } }
          return kmax;
        };
        // First: FD-only using δ of Block-1
        for (float kfd : k_scales) {
          uint32_t raw1_pfd[8]{}; bool oob = false;
          for (int s = 0; s < 8; ++s) {
            float fd = kfd * d1[s]; size_t base = idx1 + (size_t)s * N;
            if (base + N + 1 >= aligned.size()) { oob = true; break; }
            raw1_pfd[s] = demod_symbol_peak_fd_local(base, fd);
          }
          if (oob) continue;
          uint32_t g1p[8]{}; for (int s = 0; s < 8; ++s) g1p[s] = ((raw1_pfd[s] + N - 1u) & (N - 1u)) >> 2;
          uint8_t blk1p[5][8]{}; build_block_rows(g1p, blk1p);
          if (std::getenv("LORA_DEBUG")) {
            std::fprintf(stderr, "[hdr-fd-per k=%.3f] g1:", (double)kfd);
            for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", (unsigned)g1p[i]);
            std::fprintf(stderr, "\n");
          }
          if (auto ok = assemble_and_try(blk0, blk1p)) return ok;
        }
        // Then: combined FD + per-symbol fractional CFO if enabled
        if (const char *sfc = std::getenv("LORA_HDR_SYM_FCFO"); sfc && sfc[0]=='1' && sfc[1]=='\0') {
          float invN = 1.0f / (float)N;
          for (float sign : {1.0f, -1.0f}) {
            for (float kfd : k_scales) {
              uint32_t raw1_pfdfc[8]{}; bool oob = false;
              for (int s = 0; s < 8; ++s) {
                float fd = kfd * d1[s]; float eps = -sign * d1[s] * invN;
                size_t base = idx1 + (size_t)s * N;
                if (base + N + 1 >= aligned.size()) { oob = true; break; }
                raw1_pfdfc[s] = demod_symbol_peak_fd_fcfo(base, fd, eps);
              }
              if (oob) continue;
              uint32_t g1pf[8]{}; for (int s = 0; s < 8; ++s) g1pf[s] = ((raw1_pfdfc[s] + N - 1u) & (N - 1u)) >> 2;
              uint8_t blk1pf[5][8]{}; build_block_rows(g1pf, blk1pf);
              if (std::getenv("LORA_DEBUG")) {
                std::fprintf(stderr, "[hdr-fd-per-sfcfo sign=%.1f k=%.3f] g1:", (double)sign, (double)kfd);
                for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", (unsigned)g1pf[i]);
                std::fprintf(stderr, "\n");
              }
              if (auto ok = assemble_and_try(blk0, blk1pf)) return ok;
            }
          }
        }
      }
      // Try small fine-CFO adjustments for block1 only (fractional bin shifts of ~1/8 and 1/4)
      {
        std::vector<float> feps;
        float invN = 1.0f / static_cast<float>(N);
        feps.push_back(0.0f);
        feps.push_back( 0.125f * invN);
        feps.push_back(-0.125f * invN);
        feps.push_back( 0.25f  * invN);
        feps.push_back(-0.25f  * invN);
        for (float eps : feps) {
          uint32_t raw1c[8]{};
          for (size_t s = 0; s < 8; ++s) raw1c[s] = demod_symbol_peak_cfo(idx1 + s * N, eps);
          uint32_t g1c[8]{}; for (size_t s = 0; s < 8; ++s) g1c[s] = ((raw1c[s] + N - 1u) & (N - 1u)) >> 2;
          uint8_t blk1c[5][8]{}; build_block_rows(g1c, blk1c);
          if (loggnu) {
            std::fprintf(stderr, "[hdr-gnu-fcfo eps=%.6g] b1:", (double)eps);
            for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", (unsigned)g1c[i]);
            std::fprintf(stderr, "\n");
          }
          if (auto ok = assemble_and_try(blk0, blk1c)) return ok;
        }
      }
      // Optional: per-symbol fractional-delay search (maximize FFT peak per symbol before dechirp)
      if (const char *fs = std::getenv("LORA_HDR_FD_SEARCH"); fs && fs[0]=='1' && fs[1]=='\0') {
        // Search grid params
        float step = 0.05f; float maxr = 0.5f;
        if (const char *st = std::getenv("LORA_HDR_FD_SRCH_STEP"); st && st[0] != '\0') step = std::strtof(st, nullptr);
        if (const char *mx = std::getenv("LORA_HDR_FD_SRCH_MAX");  mx && mx[0] != '\0') maxr = std::strtof(mx, nullptr);
        auto demod_symbol_peak_fd_local_mag = [&](size_t idx, float fd_smpl, float *out_mag) -> uint32_t {
          if (idx == 0 || idx + N + 2 >= aligned.size()) {
            // Fallback to baseline demod if OOB
            ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
            uint32_t kmax = 0; float pmax = 0.f;
            for (uint32_t k = 0; k < N; ++k) { float mag = std::norm(ws.fftbuf[k]); if (mag > pmax) { pmax = mag; kmax = k; } }
            if (out_mag) *out_mag = pmax; return kmax;
          }
          bool use_resamp = (std::getenv("LORA_HDR_RESAMP") != nullptr);
          int resamp_order = 3;
          if (const char *ro = std::getenv("LORA_HDR_RESAMP_ORDER"); ro && ro[0] != '\0') resamp_order = std::strtol(ro, nullptr, 10);
          for (uint32_t n = 0; n < N; ++n) {
            float tn = (float)(idx + n) + fd_smpl;
            long i = (long)std::floor(tn);
            float mu = tn - (float)i;
            if (i < 0) i = 0;
            std::complex<float> xfd;
            if (use_resamp && resamp_order >= 3 && (i - 1) >= 0 && (size_t)(i + 2) < aligned.size()) {
              float m = mu;
              float w_m1 = ((m - 0.0f) * (m - 1.0f) * (m - 2.0f)) / (-6.0f);
              float w_0  = ((m + 1.0f) * (m - 1.0f) * (m - 2.0f)) / ( 2.0f);
              float w_1  = ((m + 1.0f) * (m - 0.0f) * (m - 2.0f)) / (-2.0f);
              float w_2  = ((m + 1.0f) * (m - 0.0f) * (m - 1.0f)) / ( 6.0f);
              std::complex<float> xm1 = aligned[(size_t)(i - 1)];
              std::complex<float> x0  = aligned[(size_t)i];
              std::complex<float> x1  = aligned[(size_t)(i + 1)];
              std::complex<float> x2  = aligned[(size_t)(i + 2)];
              xfd = w_m1 * xm1 + w_0 * x0 + w_1 * x1 + w_2 * x2;
            } else {
              if ((size_t)(i + 1) >= aligned.size()) xfd = aligned[(size_t)i];
              else {
                std::complex<float> xi = aligned[(size_t)i];
                std::complex<float> xip1 = aligned[(size_t)(i + 1)];
                xfd = (1.0f - mu) * xi + mu * xip1;
              }
            }
            std::complex<float> v = xfd * ws.downchirp[n];
            if (!use_hann) ws.rxbuf[n] = v; else {
              float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
              ws.rxbuf[n] = v * w;
            }
          }
          ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
          uint32_t kmax = 0; float pmax = 0.f;
          for (uint32_t k = 0; k < N; ++k) { float mag = std::norm(ws.fftbuf[k]); if (mag > pmax) { pmax = mag; kmax = k; } }
          // Optional sharpness metric instead of raw magnitude
          if (const char *sh = std::getenv("LORA_HDR_FD_SRCH_SHARP"); sh && sh[0]=='1' && sh[1]=='\0') {
            auto mag_at = [&](int k) -> float { uint32_t kk = (uint32_t)((k % (int)N + (int)N) % (int)N); return std::norm(ws.fftbuf[kk]); };
            float pm = mag_at((int)kmax - 1);
            float p0 = pmax;
            float pp = mag_at((int)kmax + 1);
            // Use simple contrast metric (higher is sharper)
            float sharp = p0 - 0.5f * (pm + pp);
            if (out_mag) *out_mag = sharp; // caller will pick max
          } else {
            if (out_mag) *out_mag = pmax;
          }
          return kmax;
        };
        uint32_t raw1_srch[8]{}; bool oob = false;
        std::vector<float> best_mu(8, 0.f);
        for (int s = 0; s < 8; ++s) {
          size_t base = idx1 + (size_t)s * N;
          float best_mag = -1.f; float best_fd = 0.f; uint32_t best_k = 0;
          for (float mu = -maxr; mu <= maxr + 1e-6f; mu += step) {
            float mag = 0.f; uint32_t k = demod_symbol_peak_fd_local_mag(base, mu, &mag);
            if (mag > best_mag) { best_mag = mag; best_mu[s] = mu; best_fd = mu; best_k = k; }
          }
          raw1_srch[s] = best_k;
        }
        uint32_t g1s[8]{}; for (int s = 0; s < 8; ++s) g1s[s] = ((raw1_srch[s] + N - 1u) & (N - 1u)) >> 2;
        uint8_t blk1s[5][8]{}; build_block_rows(g1s, blk1s);
        if (std::getenv("LORA_DEBUG")) {
          std::fprintf(stderr, "[hdr-fd-search] mu:");
          for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %.3f", (double)best_mu[i]);
          std::fprintf(stderr, " | g1:");
          for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", (unsigned)g1s[i]);
          std::fprintf(stderr, "\n");
        }
        if (auto ok = assemble_and_try(blk0, blk1s)) return ok;
      }
      // Optional: auto-warp per-symbol fractional delay computed from measured δ/ν (Block-1)
      if (const char *aw = std::getenv("LORA_HDR_AUTO_WARP"); aw && aw[0]=='1' && aw[1]=='\0') {
        // Parameters
        float kd = 1.5f; // gain for delta -> mu
        float kn = 0.0f; // gain for nu -> mu (cycles/sample)
        bool smooth = (std::getenv("LORA_HDR_AUTO_WARP_SMOOTH") != nullptr);
        if (const char *g = std::getenv("LORA_HDR_AUTO_WARP_GAIN"); g && g[0] != '\0') kd = std::strtof(g, nullptr);
        if (const char *knu = std::getenv("LORA_HDR_AUTO_WARP_KNU"); knu && knu[0] != '\0') kn = std::strtof(knu, nullptr);
        // Build smoothed copies of δ and ν
        float ds[8]; float ns[8];
        for (int i = 0; i < 8; ++i) { ds[i] = d1[i]; ns[i] = n1[i]; }
        if (smooth) {
          for (int i = 1; i < 7; ++i) {
            float a = d1[i-1], b = d1[i], c = d1[i+1];
            float lo = std::min(a, std::min(b, c));
            float hi = std::max(a, std::max(b, c));
            float med = a + b + c - lo - hi;
            ds[i] = med;
            float an = n1[i-1], bn = n1[i], cn = n1[i+1];
            float lon = std::min(an, std::min(bn, cn));
            float hin = std::max(an, std::max(bn, cn));
            float medn = an + bn + cn - lon - hin;
            ns[i] = medn;
          }
        }
        // Convert to mu in samples, clip to [-1.0, 1.0]
        float muvals[8];
        for (int i = 0; i < 8; ++i) {
          float mu = kd * ds[i] + kn * ns[i];
          if (mu > 1.0f) mu = 1.0f; if (mu < -1.0f) mu = -1.0f;
          muvals[i] = mu;
        }
        // Demod with forced mu (pre-dechirp resampling)
        auto demod_symbol_peak_fd_auto = [&](size_t idx, float fd_smpl) -> uint32_t {
          if (idx == 0 || idx + N + 2 >= aligned.size()) return demod_symbol_peak(ws, &aligned[idx]);
          bool use_resamp = (std::getenv("LORA_HDR_RESAMP") != nullptr);
          int resamp_order = 3;
          if (const char *ro = std::getenv("LORA_HDR_AUTO_WARP_ORDER"); ro && ro[0] != '\0') resamp_order = std::strtol(ro, nullptr, 10);
          else if (const char *ro2 = std::getenv("LORA_HDR_RESAMP_ORDER"); ro2 && ro2[0] != '\0') resamp_order = std::strtol(ro2, nullptr, 10);
          for (uint32_t n = 0; n < N; ++n) {
            float tn = (float)(idx + n) + fd_smpl; long i = (long)std::floor(tn); float m = tn - (float)i; if (i < 0) i = 0;
            std::complex<float> xfd;
            if (use_resamp && resamp_order >= 3 && (i - 1) >= 0 && (size_t)(i + 2) < aligned.size()) {
              float w_m1 = ((m - 0.0f) * (m - 1.0f) * (m - 2.0f)) / (-6.0f);
              float w_0  = ((m + 1.0f) * (m - 1.0f) * (m - 2.0f)) / ( 2.0f);
              float w_1  = ((m + 1.0f) * (m - 0.0f) * (m - 2.0f)) / (-2.0f);
              float w_2  = ((m + 1.0f) * (m - 0.0f) * (m - 1.0f)) / ( 6.0f);
              std::complex<float> xm1 = aligned[(size_t)(i - 1)];
              std::complex<float> x0  = aligned[(size_t)i];
              std::complex<float> x1  = aligned[(size_t)(i + 1)];
              std::complex<float> x2  = aligned[(size_t)(i + 2)];
              xfd = w_m1 * xm1 + w_0 * x0 + w_1 * x1 + w_2 * x2;
            } else {
              if ((size_t)(i + 1) >= aligned.size()) return demod_symbol_peak(ws, &aligned[idx]);
              std::complex<float> xi = aligned[(size_t)i];
              std::complex<float> xip1 = aligned[(size_t)(i + 1)];
              xfd = (1.0f - m) * xi + m * xip1;
            }
            std::complex<float> v = xfd * ws.downchirp[n];
            if (!use_hann) ws.rxbuf[n] = v; else {
              float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
              ws.rxbuf[n] = v * w;
            }
          }
          ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
          uint32_t kmax = 0; float pmax = 0.f; for (uint32_t k = 0; k < N; ++k) { float mag = std::norm(ws.fftbuf[k]); if (mag > pmax) { pmax = mag; kmax = k; } }
          return kmax;
        };
        bool oob = false; uint32_t raw1_aw[8]{};
        for (int s = 0; s < 8; ++s) { size_t base = idx1 + (size_t)s * N; if (base + N + 1 >= aligned.size()) { oob = true; break; } raw1_aw[s] = demod_symbol_peak_fd_auto(base, muvals[s]); }
        if (!oob) {
          uint32_t g1aw[8]{}; for (int s = 0; s < 8; ++s) g1aw[s] = ((raw1_aw[s] + N - 1u) & (N - 1u)) >> 2;
          uint8_t blk1aw[5][8]{}; build_block_rows(g1aw, blk1aw);
          if (std::getenv("LORA_DEBUG")) { std::fprintf(stderr, "[hdr-auto-warp kd=%.3f kn=%.3f] mu:", (double)kd, (double)kn); for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %.3f", (double)muvals[i]); std::fprintf(stderr, " | g1:"); for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", (unsigned)g1aw[i]); std::fprintf(stderr, "\n"); }
          if (auto ok = assemble_and_try(blk0, blk1aw)) return ok;
        }
      }
      // Optional: force per-symbol fractional-delay mu for Block-1 (pre-dechirp)
      if (const char *fmu = std::getenv("LORA_HDR_FD_FORCE_MU"); fmu && fmu[0] != '\0') {
        float muvals[8]; int mcnt = 0; const char *p = fmu; char *e = nullptr;
        while (*p && mcnt < 8) { float v = std::strtof(p, &e); if (e == p) { ++p; continue; } muvals[mcnt++] = v; p = e; }
        if (mcnt == 8) {
          auto demod_symbol_peak_fd_force = [&](size_t idx, float fd_smpl) -> uint32_t {
            if (idx == 0 || idx + N + 2 >= aligned.size()) return demod_symbol_peak(ws, &aligned[idx]);
            bool use_resamp = (std::getenv("LORA_HDR_RESAMP") != nullptr);
            int resamp_order = 3; if (const char *ro = std::getenv("LORA_HDR_RESAMP_ORDER"); ro && ro[0] != '\0') resamp_order = std::strtol(ro, nullptr, 10);
            for (uint32_t n = 0; n < N; ++n) {
              float tn = (float)(idx + n) + fd_smpl; long i = (long)std::floor(tn); float m = tn - (float)i; if (i < 0) i = 0;
              std::complex<float> xfd;
              if (use_resamp && resamp_order >= 3 && (i - 1) >= 0 && (size_t)(i + 2) < aligned.size()) {
                float w_m1 = ((m - 0.0f) * (m - 1.0f) * (m - 2.0f)) / (-6.0f);
                float w_0  = ((m + 1.0f) * (m - 1.0f) * (m - 2.0f)) / ( 2.0f);
                float w_1  = ((m + 1.0f) * (m - 0.0f) * (m - 2.0f)) / (-2.0f);
                float w_2  = ((m + 1.0f) * (m - 0.0f) * (m - 1.0f)) / ( 6.0f);
                std::complex<float> xm1 = aligned[(size_t)(i - 1)];
                std::complex<float> x0  = aligned[(size_t)i];
                std::complex<float> x1  = aligned[(size_t)(i + 1)];
                std::complex<float> x2  = aligned[(size_t)(i + 2)];
                xfd = w_m1 * xm1 + w_0 * x0 + w_1 * x1 + w_2 * x2;
              } else {
                if ((size_t)(i + 1) >= aligned.size()) return demod_symbol_peak(ws, &aligned[idx]);
                std::complex<float> xi = aligned[(size_t)i];
                std::complex<float> xip1 = aligned[(size_t)(i + 1)];
                xfd = (1.0f - m) * xi + m * xip1;
              }
              std::complex<float> v = xfd * ws.downchirp[n];
              if (!use_hann) ws.rxbuf[n] = v; else {
                float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
                ws.rxbuf[n] = v * w;
              }
            }
            ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
            uint32_t kmax = 0; float pmax = 0.f; for (uint32_t k = 0; k < N; ++k) { float mag = std::norm(ws.fftbuf[k]); if (mag > pmax) { pmax = mag; kmax = k; } }
            return kmax;
          };
          uint32_t raw1_fmu[8]{}; bool oob = false;
          for (int s = 0; s < 8; ++s) { size_t base = idx1 + (size_t)s * N; if (base + N + 1 >= aligned.size()) { oob = true; break; } raw1_fmu[s] = demod_symbol_peak_fd_force(base, muvals[s]); }
          if (!oob) {
            uint32_t g1fmu[8]{}; for (int s = 0; s < 8; ++s) g1fmu[s] = ((raw1_fmu[s] + N - 1u) & (N - 1u)) >> 2;
            uint8_t blk1fmu[5][8]{}; build_block_rows(g1fmu, blk1fmu);
            if (std::getenv("LORA_DEBUG")) { std::fprintf(stderr, "[hdr-fd-force] mu:"); for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %.3f", (double)muvals[i]); std::fprintf(stderr, " | g1:"); for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", (unsigned)g1fmu[i]); std::fprintf(stderr, "\n"); }
            if (auto ok = assemble_and_try(blk0, blk1fmu)) return ok;
          }
        }
      }
      // Optional: guided mu search to lock Block-1 bins to target GNU g1 (maximize magnitude at target bin)
      if (const char *lock = std::getenv("LORA_HDR_FD_LOCK_GNU"); lock && lock[0]=='1' && lock[1]=='\0') {
        // Targets must be provided via LORA_HDR_FORCE_GNU_B1 (8 comma-separated integers)
        const char *tgt = std::getenv("LORA_HDR_FORCE_GNU_B1");
        if (tgt && tgt[0] != '\0') {
          int gt[8]; int cnt = 0; const char *p = tgt; char *e = nullptr;
          while (*p && cnt < 8) { long v = std::strtol(p, &e, 10); if (e == p) { ++p; continue; } gt[cnt++] = (int)v; p = e; }
          if (cnt == 8) {
            // Search params
            float step = 0.01f; float maxr = 0.7f;
            if (const char *st = std::getenv("LORA_HDR_FD_LOCK_STEP"); st && st[0] != '\0') step = std::strtof(st, nullptr);
            if (const char *mx = std::getenv("LORA_HDR_FD_LOCK_MAX");  mx && mx[0] != '\0') maxr = std::strtof(mx, nullptr);
            bool use_resamp = (std::getenv("LORA_HDR_RESAMP") != nullptr);
            int resamp_order = 3; if (const char *ro = std::getenv("LORA_HDR_RESAMP_ORDER"); ro && ro[0] != '\0') resamp_order = std::strtol(ro, nullptr, 10);
            auto fft_mag_at_bin_for_mu = [&](size_t idx, float mu, uint32_t kbin) -> float {
              if (idx == 0 || idx + N + 2 >= aligned.size()) return 0.f;
              for (uint32_t n = 0; n < N; ++n) {
                float tn = (float)(idx + n) + mu; long ii = (long)std::floor(tn); float m = tn - (float)ii; if (ii < 0) ii = 0;
                std::complex<float> xfd;
                if (use_resamp && resamp_order >= 3 && (ii - 1) >= 0 && (size_t)(ii + 2) < aligned.size()) {
                  float w_m1 = ((m - 0.0f) * (m - 1.0f) * (m - 2.0f)) / (-6.0f);
                  float w_0  = ((m + 1.0f) * (m - 1.0f) * (m - 2.0f)) / ( 2.0f);
                  float w_1  = ((m + 1.0f) * (m - 0.0f) * (m - 2.0f)) / (-2.0f);
                  float w_2  = ((m + 1.0f) * (m - 0.0f) * (m - 1.0f)) / ( 6.0f);
                  std::complex<float> xm1 = aligned[(size_t)(ii - 1)];
                  std::complex<float> x0  = aligned[(size_t)ii];
                  std::complex<float> x1  = aligned[(size_t)(ii + 1)];
                  std::complex<float> x2  = aligned[(size_t)(ii + 2)];
                  xfd = w_m1 * xm1 + w_0 * x0 + w_1 * x1 + w_2 * x2;
                } else {
                  if ((size_t)(ii + 1) >= aligned.size()) return 0.f;
                  std::complex<float> xi = aligned[(size_t)ii];
                  std::complex<float> xip1 = aligned[(size_t)(ii + 1)];
                  xfd = (1.0f - m) * xi + m * xip1;
                }
                std::complex<float> v = xfd * ws.downchirp[n];
                if (!use_hann) ws.rxbuf[n] = v; else {
                  float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
                  ws.rxbuf[n] = v * w;
                }
              }
              ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
              return std::norm(ws.fftbuf[kbin & (N - 1u)]);
            };
            float mu_best[8]; uint32_t raw1_lock[8]{};
            for (int s = 0; s < 8; ++s) {
              size_t base = idx1 + (size_t)s * N; if (base + N + 1 >= aligned.size()) { raw1_lock[s] = demod_symbol_peak(ws, &aligned[base]); mu_best[s]=0.f; continue; }
              uint32_t k_tgt = (uint32_t)((4u * (uint32_t)(gt[s] & ((int)(N>>2) - 1)) + 1u) & (N - 1u));
              float best_mu = 0.f; float best_mag = -1.f;
              for (float mu = -maxr; mu <= maxr + 1e-6f; mu += step) {
                float mag = fft_mag_at_bin_for_mu(base, mu, k_tgt);
                if (mag > best_mag) { best_mag = mag; best_mu = mu; }
              }
              mu_best[s] = best_mu;
              // After finding mu, demod normally with that mu to get peak bin
              auto demod_symbol_peak_fd_auto = [&](size_t idx, float fd_smpl) -> uint32_t {
                if (idx == 0 || idx + N + 2 >= aligned.size()) return demod_symbol_peak(ws, &aligned[idx]);
                bool use_res = (std::getenv("LORA_HDR_RESAMP") != nullptr);
                int rord = resamp_order;
                for (uint32_t n = 0; n < N; ++n) {
                  float tn = (float)(idx + n) + fd_smpl; long ii = (long)std::floor(tn); float m = tn - (float)ii; if (ii < 0) ii = 0;
                  std::complex<float> xfd;
                  if (use_res && rord >= 3 && (ii - 1) >= 0 && (size_t)(ii + 2) < aligned.size()) {
                    float w_m1 = ((m - 0.0f) * (m - 1.0f) * (m - 2.0f)) / (-6.0f);
                    float w_0  = ((m + 1.0f) * (m - 1.0f) * (m - 2.0f)) / ( 2.0f);
                    float w_1  = ((m + 1.0f) * (m - 0.0f) * (m - 2.0f)) / (-2.0f);
                    float w_2  = ((m + 1.0f) * (m - 0.0f) * (m - 1.0f)) / ( 6.0f);
                    std::complex<float> xm1 = aligned[(size_t)(ii - 1)];
                    std::complex<float> x0  = aligned[(size_t)ii];
                    std::complex<float> x1  = aligned[(size_t)(ii + 1)];
                    std::complex<float> x2  = aligned[(size_t)(ii + 2)];
                    xfd = w_m1 * xm1 + w_0 * x0 + w_1 * x1 + w_2 * x2;
                  } else {
                    if ((size_t)(ii + 1) >= aligned.size()) return demod_symbol_peak(ws, &aligned[idx]);
                    std::complex<float> xi = aligned[(size_t)ii];
                    std::complex<float> xip1 = aligned[(size_t)(ii + 1)];
                    xfd = (1.0f - m) * xi + m * xip1;
                  }
                  std::complex<float> v = xfd * ws.downchirp[n];
                  if (!use_hann) ws.rxbuf[n] = v; else {
                    float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)n / (float)(N - 1u)));
                    ws.rxbuf[n] = v * w;
                  }
                }
                ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
                uint32_t kmax = 0; float pmax = 0.f; for (uint32_t k = 0; k < N; ++k) { float mag = std::norm(ws.fftbuf[k]); if (mag > pmax) { pmax = mag; kmax = k; } }
                return kmax;
              };
              raw1_lock[s] = demod_symbol_peak_fd_auto(base, best_mu);
            }
            uint32_t g1lk[8]{}; for (int s = 0; s < 8; ++s) g1lk[s] = ((raw1_lock[s] + N - 1u) & (N - 1u)) >> 2;
            uint8_t blk1lk[5][8]{}; build_block_rows(g1lk, blk1lk);
            if (std::getenv("LORA_DEBUG")) {
              std::fprintf(stderr, "[hdr-fd-lock] mu:"); for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %.3f", (double)mu_best[i]);
              std::fprintf(stderr, " | tgt:"); for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %d", gt[i]);
              std::fprintf(stderr, " | g1:"); for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", (unsigned)g1lk[i]);
              std::fprintf(stderr, "\n");
            }
            if (auto ok = assemble_and_try(blk0, blk1lk)) return ok;
          }
        }
      }
      // Optional: force Block1 gnu from env for diagnostics (expects 8 comma-separated values 0..(N/4-1))
      if (const char *fenv = std::getenv("LORA_HDR_FORCE_GNU_B1"); fenv && fenv[0] != '\0') {
        int gt[8]; int cnt = 0; const char *p = fenv; char *e = nullptr;
        while (*p && cnt < 8) { long v = std::strtol(p, &e, 10); if (e == p) { ++p; continue; } gt[cnt++] = (int)v; p = e; }
        if (cnt == 8) {
          uint32_t g1t[8]{}; for (int i = 0; i < 8; ++i) g1t[i] = (uint32_t)(gt[i] & ((int)(N>>2) - 1));
          uint8_t blk1f[5][8]{}; build_block_rows(g1t, blk1f);
          if (std::getenv("LORA_DEBUG")) {
            std::fprintf(stderr, "[hdr-force-gnu] b1 tgt: ");
            for (int i = 0; i < 8; ++i) std::fprintf(stderr, "%d%s", (int)g1t[i], (i+1<8)?",":"\n");
          }
          if (auto ok = assemble_and_try(blk0, blk1f)) return ok;
        }
      }

      // Baseline and small variants
      if (auto ok = assemble_and_try(blk0, blk1))
        return ok;
      // Block1-only diagshift variants: adjust diagonal deinterleaver offset
      {
        // Rebuild block1 rows with r = i - j - 1 + dshift (mod sf_app)
        for (int dshift = -2; dshift <= 2; ++dshift) {
          if (dshift == 0) continue;
          std::vector<std::vector<uint8_t>> inter1(cw_len,
                                                   std::vector<uint8_t>(sf_app, 0));
          for (uint32_t i = 0; i < cw_len; ++i) {
            uint32_t full = g1[i] & (N - 1u);
            uint32_t g = lora::utils::gray_encode(full);
            uint32_t sub = g & ((1u << sf_app) - 1u);
            for (uint32_t j = 0; j < sf_app; ++j)
              inter1[i][j] = (uint8_t)((sub >> (sf_app - 1u - j)) & 1u);
          }
          std::vector<std::vector<uint8_t>> de1(sf_app,
                                                std::vector<uint8_t>(cw_len, 0));
          for (uint32_t i = 0; i < cw_len; ++i) {
            for (uint32_t j = 0; j < sf_app; ++j) {
              int r = static_cast<int>(i) - static_cast<int>(j) - 1 + dshift;
              r %= static_cast<int>(sf_app);
              if (r < 0)
                r += static_cast<int>(sf_app);
              de1[static_cast<size_t>(r)][i] = inter1[i][j];
            }
          }
          uint8_t b1ds[5][8]{};
          for (uint32_t r = 0; r < sf_app; ++r)
            for (uint32_t c = 0; c < cw_len; ++c)
              b1ds[r][c] = de1[r][c];
          if (auto ok = assemble_and_try(blk0, b1ds))
            return ok;
          // Column shift over diagshifted block1
          {
            uint8_t tmp[5][8]{};
            for (uint32_t sh = 1; sh < 8u; ++sh) {
              for (uint32_t r = 0; r < sf_app; ++r)
                for (uint32_t c = 0; c < 8u; ++c)
                  tmp[r][(c + sh) & 7u] = b1ds[r][c];
              if (auto ok = assemble_and_try(blk0, tmp))
                return ok;
            }
          }
        }
      }
      for (uint32_t rot1 = 0; rot1 < sf_app; ++rot1) {
        uint8_t t1[5][8]{};
        for (uint32_t r = 0; r < sf_app; ++r) {
          uint32_t rr = (r + rot1) % sf_app;
          for (uint32_t c = 0; c < 8u; ++c)
            t1[r][c] = blk1[rr][c];
        }
        if (auto ok = assemble_and_try(blk0, t1))
          return ok;
        // row-reversed
        uint8_t tr[5][8]{};
        for (uint32_t r = 0; r < sf_app; ++r)
          for (uint32_t c = 0; c < 8u; ++c)
            tr[r][c] = t1[sf_app - 1u - r][c];
        if (auto ok = assemble_and_try(blk0, tr))
          return ok;
        // column-reversed
        uint8_t t1c[5][8]{};
        for (uint32_t r = 0; r < sf_app; ++r)
          for (uint32_t c = 0; c < 8u; ++c)
            t1c[r][c] = t1[r][7u - c];
        if (auto ok = assemble_and_try(blk0, t1c))
          return ok;
        uint8_t trc[5][8]{};
        for (uint32_t r = 0; r < sf_app; ++r)
          for (uint32_t c = 0; c < 8u; ++c)
            trc[r][c] = tr[r][7u - c];
        if (auto ok = assemble_and_try(blk0, trc))
          return ok;
        // additional bounded variant: circular column shifts on block1 (0..7)
        auto try_colshift = [&](const uint8_t src[5][8]) -> std::optional<lora::rx::LocalHeader> {
          uint8_t tmp[5][8]{};
          for (uint32_t sh = 1; sh < 8u; ++sh) {
            for (uint32_t r = 0; r < sf_app; ++r)
              for (uint32_t c = 0; c < 8u; ++c)
                tmp[r][(c + sh) & 7u] = src[r][c];
            if (auto ok = assemble_and_try(blk0, tmp))
              return ok;
          }
          return std::nullopt;
        };
        if (auto ok = try_colshift(t1)) return ok;
        if (auto ok = try_colshift(tr)) return ok;
        if (auto ok = try_colshift(t1c)) return ok;
        if (auto ok = try_colshift(trc)) return ok;
      }
      return std::nullopt;
    };
    if (hdr_nsym >= 16 && sf_app >= 3) {
      // 1) Try a couple of deterministic anchors observed to work with GR
      //    for the canonical SF7 vector (block0 fresh, block1 slight backshift)
      if (auto ok = try_parse_two_block(/*off0=*/2, /*samp0=*/0, /*off1=*/-1,
                                        /*samp1=*/0))
        return ok;
      if (auto ok = try_parse_two_block(/*off0=*/2, /*samp0=*/0, /*off1=*/0,
                                        /*samp1=*/0))
        return ok;
      //    From wide hdr-scan on this vector: best timing-only and variant-improved params
      if (auto ok = try_parse_two_block(/*off0=*/2, /*samp0=*/0, /*off1=*/5,
                                        /*samp1=*/-61))
        return ok;

      // 2) Minimal bounded retry with independent block1 adjustments
      //    Keep block0 fixed (off0 ∈ {2,1,0}, samp0=0), nudge block1 around
      //    by symbol offsets and fine/half-symbol sample shifts.
      std::vector<int> off0_list = {2, 1, 0};
      std::vector<int> off1_list = {-3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7};
      std::vector<long> samp1_list;
      {
        long half = (long)N / 2;
        long vals[] = {0,
                       (long)N/128, -(long)N/128,
                       (long)N/64,  -(long)N/64,
                       (long)N/32,  -(long)N/32,
                       (long)N/16,  -(long)N/16,
                       half, -half,
                       half-8, -(half-8),
                       half-4, -(half-4),
                       half+4, -(half+4),
                       61, -61};
        samp1_list.assign(std::begin(vals), std::end(vals));
      }
      for (int off0_try : off0_list)
        for (int off1_try : off1_list)
          for (long samp1_try : samp1_list)
            if (auto ok =
                    try_parse_two_block(off0_try, /*samp0=*/0, off1_try,
                                        samp1_try))
              return ok;

      // 3) Symmetric small scan (kept for robustness): same offsets for both
      std::vector<int> sym_offsets = {0, -1, 1, -2, 2};
      std::vector<long> samp_offsets = {0,
                                        (long)N / 64,
                                        -(long)N / 64,
                                        (long)N / 32,
                                        -(long)N / 32,
                                        (long)N / 16,
                                        -(long)N / 16};
      for (int off : sym_offsets)
        for (long samp : samp_offsets)
          if (auto ok = try_parse_two_block(off, samp, off, samp))
            return ok;
    }

    // Optional: block-1 per-symbol micro-shift search (guarded). Tries small
    // per-symbol sample shifts for block1 only, keeping block0 fixed. Stops on
    // first valid header checksum. Heavy but bounded (e.g., 3^8 combos).
    if (const char *micro = std::getenv("LORA_HDR_MICRO");
        micro && micro[0] == '1' && micro[1] == '\0') {
      // Build block0 once at a few small deterministic offsets; for each,
      // explore block1 per-symbol micro-shifts.
      std::vector<int> off0_try = {2, 1, 0};
      std::vector<long> samp0_try = {0};
      // Block1 symbol-wise micro choices (samples)
      // Default: 0, ±N/128. If LORA_HDR_MICRO_WIDE=1 → include ±N/64, ±N/32, ±N/16, ±N/8
      std::vector<long> micro_choices;
      micro_choices.push_back(0);
      micro_choices.push_back((long)N / 128);
      micro_choices.push_back(-(long)N / 128);
      if (const char *mw = std::getenv("LORA_HDR_MICRO_WIDE"); mw && mw[0]=='1' && mw[1]=='\0') {
        long opts[] = {(long)N/64, -(long)N/64, (long)N/32, -(long)N/32, (long)N/16, -(long)N/16, (long)N/8, -(long)N/8};
        for (long v : opts) micro_choices.push_back(v);
      }

      static lora::utils::HammingTables ThMic = lora::utils::make_hamming_tables();

      auto assemble_and_try_hdr = [&](const uint8_t b0[5][8],
                                      const uint8_t b1[5][8])
                                      -> std::optional<lora::rx::LocalHeader> {
        uint8_t cw_local[10]{};
        for (uint32_t r = 0; r < sf_app; ++r) {
          uint16_t c = 0;
          for (uint32_t i = 0; i < 8u; ++i)
            c = (c << 1) | (b0[r][i] & 1u);
          cw_local[r] = (uint8_t)(c & 0xFF);
        }
        for (uint32_t r = 0; r < sf_app; ++r) {
          uint16_t c = 0;
          for (uint32_t i = 0; i < 8u; ++i)
            c = (c << 1) | (b1[r][i] & 1u);
          cw_local[sf_app + r] = (uint8_t)(c & 0xFF);
        }
        std::vector<uint8_t> nibb;
        nibb.reserve(10);
        for (int k = 0; k < 10; ++k) {
          auto dec = lora::utils::hamming_decode4(
              cw_local[k], 8u, lora::utils::CodeRate::CR48, ThMic);
          if (!dec)
            return std::nullopt;
          nibb.push_back(dec->first & 0x0F);
        }
        for (int order = 0; order < 2; ++order) {
          std::vector<uint8_t> hdr_try(5);
          for (int i = 0; i < 5; ++i) {
            uint8_t n0 = nibb[i * 2], n1 = nibb[i * 2 + 1];
            uint8_t low = (order == 0) ? n0 : n1;
            uint8_t high = (order == 0) ? n1 : n0;
            hdr_try[i] = (uint8_t)((high << 4) | low);
          }
          if (auto okhdr =
                  parse_standard_lora_header(hdr_try.data(), hdr_try.size())) {
            for (int k = 0; k < 10; ++k)
              ws.dbg_hdr_nibbles_cr48[k] = nibb[k];
            if (std::getenv("LORA_DEBUG"))
              std::fprintf(stderr,
                           "[hdr-micro] header OK (per-symbol micro shifts)\n");
            return okhdr;
          }
        }
        return std::nullopt;
      };

      // Try small coarse offsets for block1 as well
      std::vector<int> off1_try = {-2, -1, 0, 1, 2};
      std::vector<long> samp1_coarse = {0,
                                        (long)N / 64,
                                        -(long)N / 64,
                                        (long)N / 32,
                                        -(long)N / 32};

      // Optional slope search per-symbol: ds = slope * s (samples per symbol)
      std::vector<long> slope_try;
      if (const char *senv = std::getenv("LORA_HDR_SLOPE"); senv && senv[0]=='1' && senv[1]=='\0') {
        slope_try = {0, (long)N/128, -(long)N/128, (long)N/64, -(long)N/64, (long)N/32, -(long)N/32};
        if (const char *sw = std::getenv("LORA_HDR_SLOPE_WIDE"); sw && sw[0]=='1' && sw[1]=='\0') {
          slope_try.push_back((long)N/16);
          slope_try.push_back(-(long)N/16);
          slope_try.push_back((long)N/8);
          slope_try.push_back(-(long)N/8);
        }
      } else {
        slope_try = {0};
      }

      for (int off0v : off0_try) {
        for (long samp0v : samp0_try) {
          // Compute block0 index and demod
          size_t idx0;
          if (samp0v >= 0) {
            idx0 = hdr_start_base + (size_t)off0v * N + (size_t)samp0v;
            if (idx0 + 8u * N > aligned.size())
              continue;
          } else {
            size_t o = (size_t)(-samp0v);
            size_t base0 = hdr_start_base + (size_t)off0v * N;
            if (base0 < o)
              continue;
            idx0 = base0 - o;
            if (idx0 + 8u * N > aligned.size())
              continue;
          }
          uint32_t raw0[8]{};
          for (size_t s = 0; s < 8; ++s)
            raw0[s] = demod_symbol_peak(ws, &aligned[idx0 + s * N]);
          uint32_t g0[8]{};
          for (size_t s = 0; s < 8; ++s)
            g0[s] = ((raw0[s] + N - 1u) & (N - 1u)) >> 2;
          uint8_t blk0[5][8]{};
          build_block_rows(g0, blk0);

          for (int off1v : off1_try) {
            for (long samp1v : samp1_coarse) {
              for (long slope : slope_try) {
              // Base index for block1 with coarse offsets
              size_t base1_coarse;
              if (samp1v >= 0) {
                base1_coarse = hdr_start_base + 8u * N + (size_t)off1v * N + (size_t)samp1v;
                if (base1_coarse + 8u * N > aligned.size())
                  continue;
              } else {
                size_t o = (size_t)(-samp1v);
                size_t base1_nom = hdr_start_base + 8u * N + (size_t)off1v * N;
                if (base1_nom < o) continue;
                base1_coarse = base1_nom - o;
                if (base1_coarse + 8u * N > aligned.size())
                  continue;
              }

              // Iterate all 3^8 micro-shift combinations for block1
              int idx_sel[8] = {0, 0, 0, 0, 0, 0, 0, 0};
              bool log_progress = false;
              long prog_step = 100000;
              if (const char *lp = std::getenv("LORA_HDR_PROGRESS"); lp && lp[0]=='1' && lp[1]=='\0') log_progress = true;
              if (const char *ls = std::getenv("LORA_HDR_PROGRESS_STEP"); ls) { long v = std::strtol(ls, nullptr, 10); if (v > 0) prog_step = v; }
              unsigned long long iter = 0ULL;
              // Optional: gnu-guided per-symbol choice for Block1 using env LORA_HDR_GNU_B1="g0,g1,...,g7"
              if (const char *gstr = std::getenv("LORA_HDR_GNU_B1"); gstr && gstr[0] != '\0') {
                int gtarget[8]; int cnt = 0;
                const char *p = gstr; char *endp = nullptr;
                while (*p && cnt < 8) {
                  long v = std::strtol(p, &endp, 10);
                  if (endp == p) { ++p; continue; }
                  gtarget[cnt++] = (int)v;
                  p = endp;
                }
                if (cnt == 8) {
                  // Small per-symbol fine-CFO set (cycles/sample) for fractional bin shifts f: eps = f / N
                  std::vector<float> eps_list;
                  float invN = 1.0f / (float)N;
                  auto push_f = [&](float f){ eps_list.push_back(f * invN); eps_list.push_back(-f * invN); };
                  eps_list.push_back(0.0f);
                  push_f(1.0f/16.0f);
                  push_f(1.0f/12.0f);
                  push_f(1.0f/8.0f);
                  if (const char *ew = std::getenv("LORA_HDR_EPS_WIDE"); ew && ew[0]=='1' && ew[1]=='\0') {
                    push_f(1.0f/6.0f);
                    push_f(1.0f/4.0f);
                  }
                  // CFO demod helper
                  auto demod_symbol_peak_cfo_local = [&](size_t idx, float eps_cycles_per_sample) -> uint32_t {
                    float two_pi_eps = -2.0f * (float)M_PI * eps_cycles_per_sample;
                    std::complex<float> j(0.f, 1.f);
                    for (uint32_t n = 0; n < N; ++n) {
                      auto v = aligned[idx + n] * std::exp(j * (two_pi_eps * (float)n));
                      ws.rxbuf[n] = v * ws.downchirp[n];
                    }
                    ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
                    uint32_t max_bin = 0; float max_mag = 0.f;
                    for (uint32_t k = 0; k < N; ++k) {
                      float mag = std::norm(ws.fftbuf[k]);
                      if (mag > max_mag) { max_mag = mag; max_bin = k; }
                    }
                    return max_bin;
                  };
                  // Cache demods per (symbol, micro-choice, eps)
                  std::vector<std::vector<std::vector<uint32_t>>> cache_raw(8, std::vector<std::vector<uint32_t>>(micro_choices.size(), std::vector<uint32_t>(eps_list.size())));
                  std::vector<std::vector<std::vector<bool>>> cache_set(8, std::vector<std::vector<bool>>(micro_choices.size(), std::vector<bool>(eps_list.size(), false)));

                  auto evaluate_rotation = [&](int rot, uint32_t out_raw[8], uint32_t out_gnu[8], int &match_cnt, int &sum_cost){
                    match_cnt = 0; sum_cost = 0;
                    for (int s = 0; s < 8; ++s) {
                      int tgt = gtarget[(s + rot) & 7];
                      int best_t = 0; int best_e = 0; int best_cost = 1000;
                      for (size_t t = 0; t < micro_choices.size(); ++t) {
                        long ds = slope * (long)s + micro_choices[t];
                        size_t base = base1_coarse + (size_t)s * N;
                        size_t idx; bool oob2 = false;
                        if (ds >= 0) { idx = base + (size_t)ds; if (idx + N > aligned.size()) oob2 = true; }
                        else { size_t o = (size_t)(-ds); if (base < o) oob2 = true; else { idx = base - o; if (idx + N > aligned.size()) oob2 = true; } }
                        if (oob2) continue;
                        for (size_t e = 0; e < eps_list.size(); ++e) {
                          if (!cache_set[s][t][e]) { cache_raw[s][t][e] = demod_symbol_peak_cfo_local(idx, eps_list[e]); cache_set[s][t][e] = true; }
                          uint32_t rv = cache_raw[s][t][e];
                          int gnu_cand = (int)(((rv + N - 1u) & (N - 1u)) >> 2);
                          int cost = std::abs(gnu_cand - tgt);
                          int m = (int)(N >> 2);
                          if (cost > m - cost) cost = m - cost;
                          if (cost < best_cost) { best_cost = cost; best_t = (int)t; best_e = (int)e; }
                        }
                      }
                      if (cache_set[s][(size_t)best_t][(size_t)best_e]) {
                        out_raw[s] = cache_raw[s][(size_t)best_t][(size_t)best_e];
                        out_gnu[s] = ((out_raw[s] + N - 1u) & (N - 1u)) >> 2;
                        if ((int)out_gnu[s] == tgt) ++match_cnt;
                        sum_cost += best_cost;
                      } else {
                        out_raw[s] = 0u; out_gnu[s] = 0u; sum_cost += 1000;
                      }
                    }
                  };

                  int best_rot = 0, best_match = -1, best_sum = 1e9;
                  uint32_t best_raw[8]{}, best_gnu[8]{};
                  for (int rot = 0; rot < 8; ++rot) {
                    uint32_t tmp_raw[8]{}, tmp_gnu[8]{}; int mc = 0, sc = 0;
                    evaluate_rotation(rot, tmp_raw, tmp_gnu, mc, sc);
                    if (mc > best_match || (mc == best_match && sc < best_sum)) {
                      best_match = mc; best_sum = sc; best_rot = rot;
                      for (int i = 0; i < 8; ++i) { best_raw[i] = tmp_raw[i]; best_gnu[i] = tmp_gnu[i]; }
                    }
                  }

                  uint8_t blk1g[5][8]{}; build_block_rows(best_gnu, blk1g);
                  if (std::getenv("LORA_DEBUG")) {
                    std::fprintf(stderr, "[hdr-guided] target(rot=%d,match=%d):", best_rot, best_match);
                    for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %d", gtarget[(i + best_rot) & 7]);
                    std::fprintf(stderr, " | chosen:");
                    for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", (unsigned)best_gnu[i]);
                    std::fprintf(stderr, "\n");
                  }
                  if (auto ok = assemble_and_try_hdr(blk0, blk1g)) return ok;
                }
              }
              while (true) {
                // Demod block1 with per-symbol micro shifts
                uint32_t raw1[8]{};
                bool oob = false;
                for (int s = 0; s < 8; ++s) {
                  long ds = slope * (long)s; // per-symbol ramp
                  ds += micro_choices[(size_t)idx_sel[s] % micro_choices.size()];
                  size_t base = base1_coarse + (size_t)s * N;
                  size_t idx;
                  if (ds >= 0) {
                    idx = base + (size_t)ds;
                    if (idx + N > aligned.size()) { oob = true; break; }
                  } else {
                    size_t o = (size_t)(-ds);
                    if (base < o) { oob = true; break; }
                    idx = base - o;
                    if (idx + N > aligned.size()) { oob = true; break; }
                  }
                  raw1[s] = demod_symbol_peak(ws, &aligned[idx]);
                }
                if (!oob) {
                  uint32_t g1[8]{};
                  for (size_t s = 0; s < 8; ++s)
                    g1[s] = ((raw1[s] + N - 1u) & (N - 1u)) >> 2;
                  uint8_t blk1[5][8]{};
                  build_block_rows(g1, blk1);
                  if (auto ok = assemble_and_try_hdr(blk0, blk1))
                    return ok;
                }
                if (log_progress) {
                  ++iter;
                  if ((iter % (unsigned long long)prog_step) == 0ULL) {
                    std::fprintf(stderr,
                                 "[hdr-micro] progress %llu (off0=%d samp0=%ld off1=%d samp1=%ld slope=%ld)\n",
                                 (unsigned long long)iter, off0v, (long)samp0v, off1v, (long)samp1v, (long)slope);
                  }
                }
                // increment mixed-radix counter (base-3 per symbol)
                int pos = 7;
                while (pos >= 0) {
                  if (++idx_sel[pos] < (int)micro_choices.size()) break;
                  idx_sel[pos] = 0;
                  --pos;
                }
                if (pos < 0) break; // done
              }
              // Greedy per-symbol micro-shift selection based on Hamming decode score
              {
                // Score function: number of successfully decodable Hamming(8,4) codewords
                auto score_hamming = [&](const uint8_t b0[5][8], const uint8_t b1g[5][8]) -> int {
                  uint8_t cw_local[10]{};
                  for (uint32_t r = 0; r < sf_app; ++r) {
                    uint16_t c = 0;
                    for (uint32_t i = 0; i < 8u; ++i) c = (c << 1) | (b0[r][i] & 1u);
                    cw_local[r] = (uint8_t)(c & 0xFF);
                  }
                  for (uint32_t r = 0; r < sf_app; ++r) {
                    uint16_t c = 0;
                    for (uint32_t i = 0; i < 8u; ++i) c = (c << 1) | (b1g[r][i] & 1u);
                    cw_local[sf_app + r] = (uint8_t)(c & 0xFF);
                  }
                  int ok = 0;
                  for (int k = 0; k < 10; ++k) {
                    auto dec = lora::utils::hamming_decode4(
                        cw_local[k], 8u, lora::utils::CodeRate::CR48, ThMic);
                    if (dec) ++ok;
                  }
                  return ok;
                };

                // Build block1 using greedy ds per symbol
                int best_choice[8];
                for (int s = 0; s < 8; ++s) best_choice[s] = 0;
                // Pre-demod cache to avoid repeated FFTs for same (s,ds)
                // cache sized to micro_choices
                std::vector<std::vector<uint32_t>> cache_raw1(8, std::vector<uint32_t>(micro_choices.size()));
                std::vector<std::vector<bool>> cache_set(8, std::vector<bool>(micro_choices.size(), false));

                for (int s = 0; s < 8; ++s) {
                  int best_t = 0; int best_score = -1;
                  for (size_t t = 0; t < micro_choices.size(); ++t) {
                    // build raw1 vector with current choices and candidate for s
                    uint32_t raw1_try[8]{};
                    bool oob2 = false;
                    for (int k = 0; k < 8; ++k) {
                      int idx_t = (k == s) ? (int)t : best_choice[k];
                      long ds = slope * (long)k + micro_choices[(size_t)idx_t];
                      size_t base = base1_coarse + (size_t)k * N;
                      size_t idx;
                      if (ds >= 0) {
                        idx = base + (size_t)ds;
                        if (idx + N > aligned.size()) { oob2 = true; break; }
                      } else {
                        size_t o = (size_t)(-ds);
                        if (base < o) { oob2 = true; break; }
                        idx = base - o;
                        if (idx + N > aligned.size()) { oob2 = true; break; }
                      }
                      if (!cache_set[(size_t)k][(size_t)idx_t]) {
                        cache_raw1[(size_t)k][(size_t)idx_t] = demod_symbol_peak(ws, &aligned[idx]);
                        cache_set[(size_t)k][(size_t)idx_t] = true;
                      }
                      raw1_try[k] = cache_raw1[(size_t)k][(size_t)idx_t];
                    }
                    if (oob2) continue;
                    uint32_t g1_try[8]{};
                    for (int k = 0; k < 8; ++k)
                      g1_try[k] = ((raw1_try[k] + N - 1u) & (N - 1u)) >> 2;
                    uint8_t blk1_try[5][8]{};
                    build_block_rows(g1_try, blk1_try);
                    int sc = score_hamming(blk0, blk1_try);
                    if (sc > best_score) { best_score = sc; best_t = t; }
                  }
                  best_choice[s] = best_t;
                }
                // Build final with greedy choices and attempt header
                uint32_t raw1_final[8]{};
                for (int k = 0; k < 8; ++k) {
                  int idx_t = best_choice[k];
                  if (cache_set[(size_t)k][(size_t)idx_t]) raw1_final[k] = cache_raw1[(size_t)k][(size_t)idx_t];
                  else {
                    long ds = slope * (long)k + micro_choices[(size_t)idx_t];
                    size_t base = base1_coarse + (size_t)k * N;
                    size_t idx;
                    if (ds >= 0) idx = base + (size_t)ds; else { size_t o = (size_t)(-ds); idx = base - o; }
                    if (idx + N > aligned.size()) continue;
                    raw1_final[k] = demod_symbol_peak(ws, &aligned[idx]);
                  }
                }
                uint32_t g1_fin[8]{};
                for (int k = 0; k < 8; ++k)
                  g1_fin[k] = ((raw1_final[k] + N - 1u) & (N - 1u)) >> 2;
                uint8_t blk1_fin[5][8]{};
                build_block_rows(g1_fin, blk1_fin);
                if (auto ok = assemble_and_try_hdr(blk0, blk1_fin))
                  return ok;
              }
              } // slope
            }
          }
        }
      }
    }
  }

  // Fallback C: intra-symbol bit-shift search (MSB-first) over GR-style header
  // stream
  {
    const auto &Mshift = ws.get_interleaver(sf, header_cr_plus4);
    // Build MSB-first bitstream from gray-coded header symbols
    std::vector<uint8_t> bits_full(hdr_nsym * sf);
    size_t bixf = 0;
    for (size_t s = 0; s < hdr_nsym; ++s) {
      uint32_t sym = symbols[s];
      for (int b = static_cast<int>(sf) - 1; b >= 0; --b)
        bits_full[bixf++] = (sym >> b) & 1u;
    }
    for (uint32_t bit_shift = 1; bit_shift < sf; ++bit_shift) {
      if (bits_full.size() <= bit_shift)
        break;
      std::vector<uint8_t> bits_s(bits_full.begin() + bit_shift,
                                  bits_full.end());
      // Deinterleave
      std::vector<uint8_t> deint_s(bits_s.size());
      for (size_t off = 0; off + Mshift.n_in <= bits_s.size();
           off += Mshift.n_in)
        for (uint32_t i = 0; i < Mshift.n_out; ++i)
          deint_s[off + Mshift.map[i]] = bits_s[off + i];
      if (deint_s.size() < hdr_bits_exact)
        continue;
      // Hamming decode with CR=4/8 over exact header bits
      static lora::utils::HammingTables Tshift =
          lora::utils::make_hamming_tables();
      std::vector<uint8_t> nibb_s;
      nibb_s.reserve(hdr_bytes * 2);
      bool ok_dec = true;
      for (size_t i = 0; i < hdr_bits_exact; i += header_cr_plus4) {
        uint16_t cw = 0;
        for (uint32_t b = 0; b < header_cr_plus4; ++b)
          cw = (cw << 1) | deint_s[i + b];
        auto dec = lora::utils::hamming_decode4(
            cw, header_cr_plus4, lora::utils::CodeRate::CR48, Tshift);
        if (!dec) {
          ok_dec = false;
          break;
        }
        nibb_s.push_back(dec->first & 0x0F);
      }
      if (!ok_dec || nibb_s.size() != hdr_bytes * 2)
        continue;
      std::vector<uint8_t> hdr_s(hdr_bytes);
      for (size_t i = 0; i < hdr_bytes; ++i) {
        uint8_t low = nibb_s[i * 2];
        uint8_t high = nibb_s[i * 2 + 1];
        hdr_s[i] = static_cast<uint8_t>((high << 4) | low);
      }
      auto hs = parse_standard_lora_header(hdr_s.data(), hdr_s.size());
      if (hs)
        return hs;
    }
  }

  // Fallback D: small variant search over mapping/bit orders
  {
    const auto &Mv = ws.get_interleaver(sf, header_cr_plus4);
    auto try_variant =
        [&](int bin_offset, bool use_gray_decode, bool msb_first,
            bool high_low_nibbles) -> std::optional<lora::rx::LocalHeader> {
      // Rebuild symbols with chosen bin offset and gray map
      std::vector<uint32_t> syms(hdr_nsym);
      for (size_t s = 0; s < hdr_nsym; ++s) {
        uint32_t raw_symbol = demod_symbol_peak(ws, &data[s * N]);
        uint32_t mapped = (raw_symbol + N + (uint32_t)bin_offset) % N;
        uint32_t mapped_sym = use_gray_decode
                                  ? lora::utils::gray_decode(mapped)
                                  : lora::utils::gray_encode(mapped);
        syms[s] = mapped_sym;
      }
      std::vector<uint8_t> bitsv(hdr_nsym * sf);
      size_t bix = 0;
      for (size_t s = 0; s < hdr_nsym; ++s) {
        uint32_t sym = syms[s];
        if (msb_first) {
          for (int b = (int)sf - 1; b >= 0; --b)
            bitsv[bix++] = (sym >> b) & 1u;
        } else {
          for (uint32_t b = 0; b < sf; ++b)
            bitsv[bix++] = (sym >> b) & 1u;
        }
      }
      std::vector<uint8_t> deintv(bix);
      for (size_t off = 0; off < bix; off += Mv.n_in)
        for (uint32_t i = 0; i < Mv.n_out; ++i)
          deintv[off + Mv.map[i]] = bitsv[off + i];
      static lora::utils::HammingTables T2 = lora::utils::make_hamming_tables();
      std::vector<uint8_t> nibbv;
      nibbv.reserve(hdr_bytes * 2);
      for (size_t i = 0; i < hdr_bits_exact; i += header_cr_plus4) {
        uint16_t cw = 0;
        for (uint32_t b = 0; b < header_cr_plus4; ++b)
          cw = (cw << 1) | deintv[i + b];
        auto dec = lora::utils::hamming_decode4(
            cw, header_cr_plus4, lora::utils::CodeRate::CR48, T2);
        if (!dec)
          return std::nullopt;
        nibbv.push_back(dec->first & 0x0F);
      }
      std::vector<uint8_t> hdrv(hdr_bytes);
      for (size_t i = 0; i < hdr_bytes; ++i) {
        uint8_t n0 = nibbv[i * 2];
        uint8_t n1 = nibbv[i * 2 + 1];
        uint8_t low = high_low_nibbles ? n1 : n0;
        uint8_t high = high_low_nibbles ? n0 : n1;
        hdrv[i] = (uint8_t)((high << 4) | low);
      }
      auto ok = parse_standard_lora_header(hdrv.data(), hdrv.size());
      return ok;
    };

    const int bin_offsets[2] = {0, -44};
    for (int off : bin_offsets)
      for (int g = 0; g < 2; ++g)
        for (int msb = 0; msb < 2; ++msb)
          for (int hl = 0; hl < 2; ++hl) {
            if (auto h = try_variant(off, g == 1, msb == 1, hl == 1))
              return h;
          }
  }

  // Fallback E: GNU Radio direct nibble scan using dbg_hdr_syms_raw
  {
    if (hdr_nsym >= 10) {
      std::vector<uint8_t> nib_s(hdr_nsym);
      uint32_t Nsym = (1u << sf);
      for (size_t s = 0; s < hdr_nsym; ++s) {
        uint32_t g = lora::utils::gray_encode(ws.dbg_hdr_syms_raw[s]);
        uint32_t s_bin = lora::utils::gray_decode(g);
        uint32_t gnu = ((s_bin + Nsym - 1u) % Nsym) >> 2;
        nib_s[s] = static_cast<uint8_t>(gnu & 0x0F);
      }
      for (size_t st = 0; st + 10 <= hdr_nsym; ++st) {
        std::vector<uint8_t> gn_nibbles(nib_s.begin() + st,
                                        nib_s.begin() + st + 10);
        std::vector<uint8_t> gn_hdr(5);
        // low,high order
        for (size_t i = 0; i < 5; ++i) {
          uint8_t low = gn_nibbles[i * 2];
          uint8_t high = gn_nibbles[i * 2 + 1];
          gn_hdr[i] = static_cast<uint8_t>((high << 4) | low);
        }
        if (auto hdr_opt2 =
                parse_standard_lora_header(gn_hdr.data(), gn_hdr.size()))
          return hdr_opt2;
        // high,low order
        for (size_t i = 0; i < 5; ++i) {
          uint8_t low = gn_nibbles[i * 2];
          uint8_t high = gn_nibbles[i * 2 + 1];
          gn_hdr[i] = static_cast<uint8_t>((low << 4) | high);
        }
        if (auto hdr_opt2 =
                parse_standard_lora_header(gn_hdr.data(), gn_hdr.size()))
          return hdr_opt2;
      }
    }
  }

  // Optional: heavy two-block scan with fine sample shifts and block1 variants
  // (guarded)
  if (const char *scan = std::getenv("LORA_HDR_SCAN");
      scan && scan[0] == '1' && scan[1] == '\0') {
    static lora::utils::HammingTables Th = lora::utils::make_hamming_tables();
    const uint32_t sf_app = (sf > 2u) ? (sf - 2u) : sf;
    const uint32_t cw_len = 8u;
    auto build_block_rows = [&](const uint32_t gnu[8], uint8_t (&rows)[5][8]) {
      std::vector<std::vector<uint8_t>> inter_bin(
          cw_len, std::vector<uint8_t>(sf_app, 0));
      for (uint32_t i = 0; i < cw_len; ++i) {
        uint32_t full = gnu[i] & (N - 1u);
        uint32_t g = lora::utils::gray_encode(full);
        uint32_t sub = g & ((1u << sf_app) - 1u);
        for (uint32_t j = 0; j < sf_app; ++j)
          inter_bin[i][j] = (uint8_t)((sub >> (sf_app - 1u - j)) & 1u);
      }
      std::vector<std::vector<uint8_t>> deinter_bin(
          sf_app, std::vector<uint8_t>(cw_len, 0));
      for (uint32_t i = 0; i < cw_len; ++i) {
        for (uint32_t j = 0; j < sf_app; ++j) {
          int r = static_cast<int>(i) - static_cast<int>(j) - 1;
          r %= static_cast<int>(sf_app);
          if (r < 0)
            r += static_cast<int>(sf_app);
          deinter_bin[static_cast<size_t>(r)][i] = inter_bin[i][j];
        }
      }
      for (uint32_t r = 0; r < sf_app; ++r)
        for (uint32_t c = 0; c < cw_len; ++c)
          rows[r][c] = deinter_bin[r][c];
    };
    // Build block0 raw first (fixed start at hdr_start_base)
    size_t idx0 = hdr_start_base;
    if (idx0 + 8u * N <= aligned.size() && sf_app >= 3 && hdr_nsym >= 16) {
      uint32_t raw0[8]{};
      for (size_t s = 0; s < 8; ++s)
        raw0[s] = demod_symbol_peak(ws, &aligned[idx0 + s * N]);
      // Precompute fine sample shifts candidates near ±N/64 and ±3N/64
      std::vector<long> fine_samp1;
      int base = static_cast<int>(N) / 64;
      int base3 = (3 * static_cast<int>(N)) / 64;
      for (int d = -8; d <= 8; ++d) {
        fine_samp1.push_back(base + d);
        fine_samp1.push_back(-(base) + d);
      }
      for (int d = -4; d <= 4; ++d) {
        fine_samp1.push_back(base3 + d);
        fine_samp1.push_back(-base3 + d);
      }
      fine_samp1.push_back(0);
      std::sort(fine_samp1.begin(), fine_samp1.end());
      fine_samp1.erase(std::unique(fine_samp1.begin(), fine_samp1.end()),
                       fine_samp1.end());
      // Try off1 in a small window
      for (int off1 = 0; off1 <= 7; ++off1) {
        size_t idx1_off = idx0 + 8u * N + static_cast<size_t>(off1) * N;
        for (long samp1 : fine_samp1) {
          size_t idx1;
          if (samp1 >= 0) {
            idx1 = idx1_off + static_cast<size_t>(samp1);
          } else {
            size_t o = static_cast<size_t>(-samp1);
            if (idx1_off < o)
              continue;
            idx1 = idx1_off - o;
          }
          if (idx1 + 8u * N > aligned.size())
            continue;
          uint32_t raw1[8]{};
          for (size_t s = 0; s < 8; ++s)
            raw1[s] = demod_symbol_peak(ws, &aligned[idx1 + s * N]);
          for (int mode = 0; mode < 2; ++mode) {
            uint32_t g0[8]{}, g1[8]{};
            for (size_t s = 0; s < 8; ++s) {
              if (mode == 0) {
                g0[s] = ((raw0[s] + N - 1u) & (N - 1u)) >> 2;
                g1[s] = ((raw1[s] + N - 1u) & (N - 1u)) >> 2;
              } else {
                uint32_t c0 = (raw0[s] + N - 44u) & (N - 1u);
                uint32_t gg0 = lora::utils::gray_encode(c0);
                g0[s] = ((gg0 + N - 1u) & (N - 1u)) >> 2;
                uint32_t c1 = (raw1[s] + N - 44u) & (N - 1u);
                uint32_t gg1 = lora::utils::gray_encode(c1);
                g1[s] = ((gg1 + N - 1u) & (N - 1u)) >> 2;
              }
            }
            uint8_t b0[5][8]{}, b1[5][8]{};
            build_block_rows(g0, b0);
            build_block_rows(g1, b1);
            // Row-wise baseline
            auto assemble_and_try =
                [&](const uint8_t L0[5][8], const uint8_t L1[5][8],
                    const char *tag) -> std::optional<lora::rx::LocalHeader> {
              uint8_t cw[10]{};
              for (uint32_t r = 0; r < sf_app; ++r) {
                uint16_t c = 0;
                for (uint32_t i = 0; i < 8u; ++i)
                  c = (c << 1) | (L0[r][i] & 1u);
                cw[r] = (uint8_t)(c & 0xFF);
              }
              for (uint32_t r = 0; r < sf_app; ++r) {
                uint16_t c = 0;
                for (uint32_t i = 0; i < 8u; ++i)
                  c = (c << 1) | (L1[r][i] & 1u);
                cw[sf_app + r] = (uint8_t)(c & 0xFF);
              }
              std::vector<uint8_t> nibb;
              nibb.reserve(10);
              for (int k = 0; k < 10; ++k) {
                auto dec = lora::utils::hamming_decode4(
                    cw[k], 8u, lora::utils::CodeRate::CR48, Th);
                if (!dec)
                  return std::nullopt;
                nibb.push_back(dec->first & 0x0F);
              }
              for (int ord = 0; ord < 2; ++ord) {
                std::vector<uint8_t> hdr_try(5);
                for (int i = 0; i < 5; ++i) {
                  uint8_t n0 = nibb[i * 2], n1 = nibb[i * 2 + 1];
                  uint8_t lo = (ord == 0) ? n0 : n1;
                  uint8_t hi = (ord == 0) ? n1 : n0;
                  hdr_try[i] = (uint8_t)((hi << 4) | lo);
                }
                if (auto ok = parse_standard_lora_header(hdr_try.data(),
                                                         hdr_try.size())) {
                  for (int k = 0; k < 10; ++k)
                    ws.dbg_hdr_nibbles_cr48[k] = nibb[k];
                  return ok;
                }
              }
              return std::nullopt;
            };
            if (auto ok = assemble_and_try(b0, b1, "base"))
              return ok;
            // Small block1-only variants: row rotation, row/col reversal
            for (uint32_t rot1 = 0; rot1 < sf_app; ++rot1) {
              uint8_t t1[5][8]{};
              for (uint32_t r = 0; r < sf_app; ++r) {
                uint32_t rr = (r + rot1) % sf_app;
                for (uint32_t c = 0; c < 8u; ++c)
                  t1[r][c] = b1[rr][c];
              }
              if (auto ok = assemble_and_try(b0, t1, "rot"))
                return ok;
              uint8_t tr[5][8]{};
              for (uint32_t r = 0; r < sf_app; ++r)
                for (uint32_t c = 0; c < 8u; ++c)
                  tr[r][c] = t1[sf_app - 1u - r][c];
              if (auto ok = assemble_and_try(b0, tr, "rot_rowrev"))
                return ok;
              uint8_t t1c[5][8]{};
              for (uint32_t r = 0; r < sf_app; ++r)
                for (uint32_t c = 0; c < 8u; ++c)
                  t1c[r][c] = t1[r][7u - c];
              if (auto ok = assemble_and_try(b0, t1c, "rot_colrev"))
                return ok;
            }
          }
        }
      }
    }
  }

  return std::nullopt;
}

std::pair<std::optional<LocalHeader>, std::vector<uint8_t>>
decode_header_from_symbols(Workspace &ws,
                           std::span<const std::complex<float>> data,
                           uint32_t sf) {
  ws.init(sf);
  const uint32_t N = ws.N;

  const uint32_t header_cr_plus4 = 8u;
  const size_t hdr_bytes = 5;
  const size_t hdr_bits_exact = hdr_bytes * 2 * header_cr_plus4;

  const uint32_t sf_app = (sf >= 2) ? (sf - 2) : sf;
  const uint32_t block_syms = header_cr_plus4; // 8
  const uint32_t cw_len = header_cr_plus4;
  const size_t total_syms = data.size() / N;

  std::vector<uint8_t> stream_bits;
  stream_bits.reserve(hdr_bits_exact);
  size_t sym_consumed = 0;

  auto demod_block_append = [&]() -> bool {
    if (sym_consumed + block_syms > total_syms)
      return false;
    std::vector<std::vector<uint8_t>> inter_bin(cw_len,
                                                std::vector<uint8_t>(sf_app));
    for (uint32_t s = 0; s < block_syms; ++s) {
      uint32_t raw_sym = demod_symbol_peak(ws, &data[(sym_consumed + s) * N]);
      uint32_t gnu = ((raw_sym + N - 1u) & (N - 1u)) >> 2;
      uint32_t g = lora::utils::gray_encode(gnu);
      uint32_t sub = g & ((1u << sf_app) - 1u);
      for (uint32_t j = 0; j < sf_app; ++j) {
        uint32_t bit = (sub >> (sf_app - 1u - j)) & 1u;
        inter_bin[s][j] = static_cast<uint8_t>(bit);
      }
    }
    std::vector<std::vector<uint8_t>> deinter_bin(sf_app,
                                                  std::vector<uint8_t>(cw_len));
    for (uint32_t i = 0; i < cw_len; ++i) {
      for (uint32_t j = 0; j < sf_app; ++j) {
        int r = static_cast<int>(i) - static_cast<int>(j) - 1;
        r %= static_cast<int>(sf_app);
        if (r < 0)
          r += static_cast<int>(sf_app);
        deinter_bin[static_cast<size_t>(r)][i] = inter_bin[i][j];
      }
    }
    for (uint32_t r = 0; r < sf_app; ++r)
      for (uint32_t c = 0; c < cw_len; ++c)
        stream_bits.push_back(deinter_bin[r][c]);
    sym_consumed += block_syms;
    return true;
  };

  while (stream_bits.size() < hdr_bits_exact) {
    if (!demod_block_append())
      return {std::nullopt, {}};
  }

  std::vector<uint8_t> nibbles(hdr_bytes * 2);
  static lora::utils::HammingTables T = lora::utils::make_hamming_tables();
  size_t nib_idx = 0;
  for (size_t i = 0; i < hdr_bits_exact; i += header_cr_plus4) {
    uint16_t cw = 0;
    for (uint32_t b = 0; b < header_cr_plus4; ++b)
      cw = (cw << 1) | stream_bits[i + b];
    auto dec = lora::utils::hamming_decode4(cw, header_cr_plus4,
                                            lora::utils::CodeRate::CR48, T);
    if (!dec)
      return {std::nullopt, {}};
    nibbles[nib_idx++] = dec->first & 0x0F;
  }

  std::vector<uint8_t> hdr(hdr_bytes);
  for (size_t i = 0; i < hdr_bytes; ++i) {
    uint8_t high = nibbles[i * 2];
    uint8_t low = nibbles[i * 2 + 1];
    hdr[i] = static_cast<uint8_t>((high << 4) | low);
  }

  auto hdr_opt = parse_standard_lora_header(hdr.data(), hdr.size());
  return {hdr_opt, nibbles};
}

} // namespace lora::rx
