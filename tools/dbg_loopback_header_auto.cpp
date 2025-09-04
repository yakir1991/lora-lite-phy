#include "lora/workspace.hpp"
#include "lora/tx/frame_tx.hpp"
#include "lora/rx/loopback_rx.hpp"
#include "lora/rx/preamble.hpp"
#include "lora/rx/decimate.hpp"
#include "lora/debug.hpp"
#include "lora/constants.hpp"
#include "lora/utils/gray.hpp"
#include <vector>
#include <complex>
#include <cstdio>

using namespace lora;

static std::vector<std::complex<float>> upsample_repeat(std::span<const std::complex<float>> x, int os) {
  if (os <= 1) return std::vector<std::complex<float>>(x.begin(), x.end());
  std::vector<std::complex<float>> y; y.reserve(x.size() * os);
  for (auto v : x) for (int i = 0; i < os; ++i) y.push_back(v);
  return y;
}

int main() {
  Workspace ws;
  uint32_t sf = 7; auto cr = lora::utils::CodeRate::CR47; size_t pre_len = 8;
  std::vector<uint8_t> payload(24); for (size_t i=0;i<payload.size();++i) payload[i] = uint8_t(i*3+7);
  lora::rx::LocalHeader hdr{ .payload_len = (uint8_t)payload.size(), .cr = cr, .has_crc = true };
  auto iq = lora::tx::frame_tx(ws, payload, sf, cr, hdr);
  ws.init(sf); uint32_t N = ws.N; size_t nsym = iq.size()/N;
  std::vector<std::complex<float>> sig((pre_len+1)*N + iq.size());
  for (size_t s=0;s<pre_len;++s) for (uint32_t n=0;n<N;++n) sig[s*N+n] = ws.upchirp[n];
  uint32_t sync_sym = lora::utils::gray_encode((uint32_t)lora::LORA_SYNC_WORD_PUBLIC);
  for (uint32_t n=0;n<N;++n) sig[pre_len*N + n] = ws.upchirp[(n + sync_sym)%N];
  std::copy(iq.begin(), iq.end(), sig.begin() + (pre_len+1)*N);
  auto sig_os4 = upsample_repeat(std::span<const std::complex<float>>(sig.data(), sig.size()), 4);
  auto span_os4 = std::span<const std::complex<float>>(sig_os4.data(), sig_os4.size());
  auto out = lora::rx::loopback_rx_header_auto(ws, span_os4, sf, cr, pre_len, true);
  // Manual path mirror for counts
  auto det = lora::rx::detect_preamble_os(ws, span_os4, sf, pre_len, {1,2,4,8});
  size_t total_syms = 0; size_t start_decim = 0; int os=0, phase=0;
  if (det) {
    os = det->os; phase = det->phase; auto decim = lora::rx::decimate_os_phase(span_os4, det->os, det->phase);
    start_decim = det->start_sample / (size_t)det->os;
    auto aligned0 = std::span<const std::complex<float>>(decim.data() + start_decim, decim.size() - start_decim);
    auto pos0 = lora::rx::detect_preamble(ws, aligned0, sf, pre_len);
    if (pos0) {
      auto cfo = lora::rx::estimate_cfo_from_preamble(ws, aligned0, sf, *pos0, pre_len);
      std::vector<std::complex<float>> comp(aligned0.size());
      float two_pi_eps = -2.0f * float(M_PI) * (*cfo);
      std::complex<float> j(0.f,1.f);
      for (size_t n = 0; n < aligned0.size(); ++n) comp[n] = aligned0[n] * std::exp(j * (two_pi_eps * float(n)));
      auto sto = lora::rx::estimate_sto_from_preamble(ws, comp, sf, *pos0, pre_len, int(ws.N/8));
      int shift = *sto; size_t aligned_start = (shift>=0) ? (*pos0 + size_t(shift)) : (*pos0 - size_t(-shift));
      auto aligned = std::span<const std::complex<float>>(comp.data() + aligned_start, comp.size() - aligned_start);
      size_t sync_start = pre_len * ws.N; auto data = std::span<const std::complex<float>>(aligned.data() + sync_start + ws.N, aligned.size() - (sync_start + ws.N));
      total_syms = data.size() / ws.N;
      printf("det.os=%d phase=%d start_decim=%zu pos0=%zu shift=%d aligned.size=%zu data.size=%zu\n",
             os, phase, start_decim, *pos0, shift, aligned.size(), data.size());
    }
  }
  printf("frame nsym=%zu, os=%d phase=%d start_decim=%zu total_syms_after_sync=%zu, last_fail=%d, ok=%d, out_len=%zu\n",
         nsym, os, phase, start_decim, total_syms, lora::debug::last_fail_step, (int)out.second, out.first.size());
  return out.second ? 0 : 1;
}
