#include "dsp.hpp"
#include <algorithm>
#include <cmath>

namespace lora::standalone {

static inline float twopi() { return 2.0f * static_cast<float>(M_PI); }

// Minimal iterative radix-2 FFT (in-place). N must be a power of two.
static void fft_inplace(std::vector<std::complex<float>>& a, bool inverse=false)
{
    const size_t N = a.size();
    if (N <= 1) return;
    // bit-reversal permutation
    size_t j = 0;
    for (size_t i = 1; i < N; ++i) {
        size_t bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= N; len <<= 1) {
        float ang = twopi() / static_cast<float>(len) * (inverse ? 1.f : -1.f);
        std::complex<float> wlen{std::cos(ang), std::sin(ang)};
        for (size_t i = 0; i < N; i += len) {
            std::complex<float> w{1.f,0.f};
            for (size_t k = 0; k < len/2; ++k) {
                std::complex<float> u = a[i + k];
                std::complex<float> v = a[i + k + len/2] * w;
                a[i + k] = u + v;
                a[i + k + len/2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse) {
        float invN = 1.0f / static_cast<float>(N);
        for (auto& x : a) x *= invN;
    }
}

ChirpRefs build_ref_chirps(uint32_t sf, uint32_t os)
{
    const uint32_t N = 1u << sf;
    const uint32_t L = N * os;
    ChirpRefs r;
    r.up.resize(L);
    r.down.resize(L);

    // Build baseband upchirp and downchirp sampled at OS
    // Phase: exp(j*2pi*(n^2/(2N)/os^2 - 0.5*n/os)) for upchirp id=0
    for (uint32_t n = 0; n < L; ++n) {
        float nf = static_cast<float>(n);
        float Nf = static_cast<float>(N);
        float osf = static_cast<float>(os);
        float phase_up = twopi() * ( (nf*nf)/(2.0f*Nf*osf*osf) - 0.5f*nf/osf );
        float phase_dn = -phase_up; // conj(upchirp)
        r.up[n] = std::complex<float>(std::cos(phase_up), std::sin(phase_up));
        r.down[n] = std::complex<float>(std::cos(phase_dn), std::sin(phase_dn));
    }
    return r;
}

std::vector<std::complex<float>> decimate_os_phase(std::span<const std::complex<float>> x, int os, int phase)
{
    std::vector<std::complex<float>> y;
    if (os <= 1) {
        y.assign(x.begin(), x.end());
        return y;
    }
    y.reserve((x.size() > static_cast<size_t>(phase)) ? (x.size() - phase + os - 1) / os : 0);
    for (size_t i = phase; i < x.size(); i += os) y.push_back(x[i]);
    return y;
}

// Compute power at integer FFT bins by correlating with complex exponentials (naive DFT)
static uint32_t argmax_bin_power(std::span<const std::complex<float>> v)
{
    const size_t N = v.size();
    if (N == 0) return 0u;
    // Precompute twiddle for k increment
    float Nf = static_cast<float>(N);
    float base = -twopi() / Nf;
    uint32_t best_k = 0; float best_p = -1.0f;
    // For LoRa we only need 0..N-1 bins
    for (uint32_t k = 0; k < N; ++k) {
        std::complex<float> acc{0.f,0.f};
        float ang_step = base * static_cast<float>(k);
        std::complex<float> w = {std::cos(ang_step), std::sin(ang_step)};
        std::complex<float> z{1.f,0.f};
        for (size_t n = 0; n < N; ++n) {
            acc += v[n] * std::conj(z);
            // z *= w;
            float zr = z.real()*w.real() - z.imag()*w.imag();
            float zi = z.real()*w.imag() + z.imag()*w.real();
            z = {zr, zi};
        }
        float p = std::norm(acc);
        if (p > best_p) { best_p = p; best_k = k; }
    }
    return best_k;
}

uint32_t demod_symbol_peak(std::span<const std::complex<float>> block,
                           std::span<const std::complex<float>> downchirp)
{
    // element-wise multiply by downchirp (dechirp)
    const size_t N = std::min(block.size(), downchirp.size());
    std::vector<std::complex<float>> tmp(N);
    for (size_t i = 0; i < N; ++i) tmp[i] = block[i] * downchirp[i];
    return argmax_bin_power(tmp);
}

uint32_t demod_symbol_peak_cfo(std::span<const std::complex<float>> block,
                               std::span<const std::complex<float>> downchirp,
                               float eps)
{
    const size_t N = std::min(block.size(), downchirp.size());
    std::vector<std::complex<float>> tmp(N);
    // Apply CFO correction: multiply by exp(-j*2pi*eps*n/N) during dechirp
    float tw = -twopi() * eps / static_cast<float>(N);
    std::complex<float> w = {std::cos(tw), std::sin(tw)};
    std::complex<float> z{1.f,0.f};
    for (size_t n = 0; n < N; ++n) {
        tmp[n] = block[n] * downchirp[n] * z; // z is exp(-j*2pi*eps*n/N)
        float zr = z.real()*w.real() - z.imag()*w.imag();
        float zi = z.real()*w.imag() + z.imag()*w.real();
        z = {zr, zi};
    }
    return argmax_bin_power(tmp);
}

uint32_t demod_symbol_peak_fft(std::span<const std::complex<float>> block,
                               std::span<const std::complex<float>> downchirp,
                               float eps)
{
    const size_t N = std::min(block.size(), downchirp.size());
    std::vector<std::complex<float>> tmp(N);
    float tw = -twopi() * eps / static_cast<float>(N);
    std::complex<float> w = {std::cos(tw), std::sin(tw)};
    std::complex<float> z{1.f,0.f};
    for (size_t n = 0; n < N; ++n) {
        tmp[n] = block[n] * downchirp[n] * z;
        float zr = z.real()*w.real() - z.imag()*w.imag();
        float zi = z.real()*w.imag() + z.imag()*w.real();
        z = {zr, zi};
    }
    fft_inplace(tmp, false);
    uint32_t best_k = 0; float best_p = -1.0f;
    for (uint32_t k = 0; k < N; ++k) {
        float p = std::norm(tmp[k]);
        if (p > best_p) { best_p = p; best_k = k; }
    }
    return best_k;
}

DemodResult demod_symbol_peak_fft_best_shift(std::span<const std::complex<float>> block,
                                             std::span<const std::complex<float>> downchirp,
                                             float eps,
                                             int max_shift)
{
    const size_t N = std::min(block.size(), downchirp.size());
    DemodResult best{0, -1.f, 0};
    for (int sh = -max_shift; sh <= max_shift; ++sh) {
        if (sh < 0) {
            size_t off = static_cast<size_t>(-sh);
            if (off + N > block.size()) continue;
            uint32_t k = demod_symbol_peak_fft(std::span<const std::complex<float>>(block.data()+off, N), downchirp, eps);
            // Estimate power around k using a tiny re-eval: not storing full spectrum, re-use inner function to approximate via DC proxy
            // For simplicity, set a placeholder power as inverse of bin index distance from 0 (coarse ranking)
            float p = 1.0f / (1.0f + std::abs(static_cast<int>(k))); 
            if (p > best.power) best = DemodResult{k, p, sh};
        } else {
            if (sh + N > block.size()) continue;
            uint32_t k = demod_symbol_peak_fft(std::span<const std::complex<float>>(block.data()+sh, N), downchirp, eps);
            float p = 1.0f / (1.0f + std::abs(static_cast<int>(k)));
            if (p > best.power) best = DemodResult{k, p, sh};
        }
    }
    return best;
}

PreambleDetectResult detect_preamble_os(std::span<const std::complex<float>> samples,
                                        uint32_t sf,
                                        size_t min_syms,
                                        std::span<const int> os_candidates)
{
    PreambleDetectResult best{};
    const uint32_t N = 1u << sf;

    for (int os : os_candidates) {
        if (os <= 0) continue;
        for (int phase = 0; phase < std::max(1, os); ++phase) {
            auto decim = decimate_os_phase(samples, os, phase);
            if (decim.size() < N * min_syms) continue;
            auto refs = build_ref_chirps(sf, 1);
            // slide by symbol length and expect bin 0 for upchirps
            const size_t L = decim.size();
            size_t i = 0;
            size_t run = 0; size_t start = 0;
            while (i + N <= L) {
                uint32_t bin = demod_symbol_peak(std::span<const std::complex<float>>(decim.data()+i, N), refs.down);
                if (bin == 0) {
                    if (run == 0) start = i;
                    run++;
                    if (run >= min_syms) {
                        best.found = true;
                        best.start_raw = static_cast<size_t>(start) * static_cast<size_t>(os) + static_cast<size_t>(phase);
                        best.os = os; best.phase = phase;
                        return best; // first match
                    }
                } else {
                    run = 0;
                }
                i += N;
            }
        }
    }
    return best;
}

float estimate_cfo_epsilon(std::span<const std::complex<float>> decim,
                           uint32_t sf,
                           size_t start_decim,
                           size_t preamble_syms)
{
    const uint32_t N = 1u << sf;
    if (start_decim + (preamble_syms+1) * N > decim.size()) return 0.f;
    // Simple method: correlate two consecutive dechirped preamble symbols at bin 0 and take phase slope
    // Here we approximate by computing the complex sum of s[n]*conj(s[n-N]) over preamble region
    std::complex<float> acc{0.f,0.f};
    size_t base = start_decim;
    for (size_t s = 1; s < preamble_syms; ++s) {
        const auto* a = decim.data() + base + s*N;
        const auto* b = decim.data() + base + (s-1)*N;
        for (uint32_t n = 0; n < N; ++n) acc += a[n] * std::conj(b[n]);
    }
    float angle = std::atan2(acc.imag(), acc.real());
    // angle per symbol; map to fractional bins: eps ~= angle/(2pi)
    float eps = angle / twopi();
    // clamp into [-0.5,0.5)
    while (eps >= 0.5f) eps -= 1.0f;
    while (eps < -0.5f) eps += 1.0f;
    return eps;
}

int estimate_cfo_integer(std::span<const std::complex<float>> decim,
                         uint32_t sf,
                         size_t start_decim,
                         size_t preamble_syms)
{
    const uint32_t N = 1u << sf;
    if (start_decim + preamble_syms*N > decim.size()) return 0;
    auto refs = build_ref_chirps(sf, 1);
    // For each preamble symbol, dechirp with downchirp and FFT; expect peak near bin 0 shifted by CFOint
    std::vector<int> peaks;
    for (size_t s = 0; s < preamble_syms; ++s) {
        size_t pos = start_decim + s*N;
        uint32_t k = demod_symbol_peak_fft(std::span<const std::complex<float>>(decim.data()+pos, N), std::span<const std::complex<float>>(refs.down.data(), N), 0.0f);
        int ci = static_cast<int>(k);
        if (ci > static_cast<int>(N)/2) ci -= static_cast<int>(N);
        peaks.push_back(ci);
    }
    // Return median to be robust to outliers
    if (peaks.empty()) return 0;
    std::nth_element(peaks.begin(), peaks.begin() + peaks.size()/2, peaks.end());
    return peaks[peaks.size()/2];
}

SymbolTypeScore classify_symbol(std::span<const std::complex<float>> block,
                                std::span<const std::complex<float>> upchirp,
                                std::span<const std::complex<float>> downchirp,
                                float eps)
{
    const size_t N = std::min({block.size(), upchirp.size(), downchirp.size()});
    if (N == 0) return {};
    // Build CFO phasor sequence
    float tw = -twopi() * eps / static_cast<float>(N);
    std::complex<float> w = {std::cos(tw), std::sin(tw)};
    std::complex<float> z{1.f,0.f};

    std::vector<std::complex<float>> tmp_u(N), tmp_d(N);
    for (size_t n = 0; n < N; ++n) {
        tmp_u[n] = block[n] * upchirp[n] * z;   // dechirp with up (detect downchirp symbol)
        tmp_d[n] = block[n] * downchirp[n] * z; // dechirp with down (detect upchirp symbol)
        float zr = z.real()*w.real() - z.imag()*w.imag();
        float zi = z.real()*w.imag() + z.imag()*w.real();
        z = {zr, zi};
    }
    // Scores: magnitude at bin 0 is a good fast proxy
    // Compute DC (bin 0) complex sum power
    auto dc_pow = [](const std::vector<std::complex<float>>& x){
        std::complex<float> acc{0.f,0.f};
        for (auto& v : x) acc += v;
        return std::norm(acc);
    };
    SymbolTypeScore s;
    s.down_score = dc_pow(tmp_u); // downchirp yields strong DC after up-dechirp
    s.up_score   = dc_pow(tmp_d); // upchirp yields strong DC after down-dechirp
    return s;
}

// Hamming(7,4): positions (MSB-first b[0]..b[6]) mapping to parity checks:
// We'll adopt conventional H matrix for (7,4): parity bits at positions 1,2,4 (1-based)
// Implement with syndrome s = H * c^T; if s != 0, flip that bit index.
uint8_t hamming74_decode_bits_msb(const uint8_t b[7], bool* err_corrected, bool* uncorrectable)
{
    // Convert MSB-first b[0]..b[6] into c[1..7] (1-based) with c[1]=b[0]
    uint8_t c[8];
    for (int i = 1; i <= 7; ++i) c[i] = b[i-1] & 1u;
    // Parity-check matrix H rows (1-based bit positions in each parity):
    auto p = [&](std::initializer_list<int> idx){ int v=0; for(int i: idx) v ^= c[i]; return v; };
    int s1 = p({1,3,5,7}); // parity for bit1
    int s2 = p({2,3,6,7}); // parity for bit2
    int s3 = p({4,5,6,7}); // parity for bit4
    int syndrome = (s3<<2) | (s2<<1) | s1; // 3-bit syndrome gives error position (1..7)
    bool corrected=false, uncor=false;
    if (syndrome != 0) {
        if (syndrome >=1 && syndrome <=7) { c[syndrome] ^= 1; corrected=true; }
        else { uncor=true; }
    }
    if (err_corrected) *err_corrected = corrected;
    if (uncorrectable) *uncorrectable = uncor;
    // Extract data bits positions 3,5,6,7 (common mapping) into a nibble MSB-first
    uint8_t d3=c[3], d5=c[5], d6=c[6], d7=c[7];
    uint8_t nibble = (d3<<3) | (d5<<2) | (d6<<1) | d7;
    return nibble;
}

// Hamming(8,4) used by LoRa CR4/8 header: codewords correspond to LUT as in gr-lora-sdr (data high nibble then 4 parity bits)
uint8_t hamming84_decode_bits_msb(const uint8_t b[8], bool* err_corrected, int* distance)
{
    // Build byte from MSB-first bits
    uint8_t x = 0;
    for (int i = 0; i < 8; ++i) x = static_cast<uint8_t>((x << 1) | (b[i] & 1u));
    // LoRa LUT as in hamming_dec_impl.cc for non-CR5: data in high nibble
    static const uint8_t CW_LUT[16] = {
        0, 23, 45, 58, 78, 89, 99, 116, 139, 156, 166, 177, 197, 210, 232, 255
    };
    int best_nib = 0; int best_diff = 9;
    for (int nib = 0; nib < 16; ++nib) {
        uint8_t cw = CW_LUT[nib];
        int diff = std::popcount(static_cast<unsigned>(cw ^ x));
        if (diff < best_diff) { best_diff = diff; best_nib = nib; if (best_diff == 0) break; }
    }
    if (err_corrected) *err_corrected = (best_diff > 0);
    if (distance) *distance = best_diff;
    return static_cast<uint8_t>(best_nib & 0x0F);
}

uint8_t hamming_payload_decode_bits_msb(const uint8_t bits8[8], int cw_len, bool* corrected, int* distance)
{
    // cw_len in [5..8], CR 4/5 uses a different LUT; others share a base LUT cropped to cw_len
    static const uint8_t LUT[16]     = {0, 23, 45, 58, 78, 89, 99, 116, 139, 156, 166, 177, 197, 210, 232, 255};
    static const uint8_t LUT_CR5[16] = {0, 24, 40, 48, 72, 80, 96, 120, 136, 144, 160, 184, 192, 216, 232, 240};
    auto popc8 = [](uint8_t x){ x = x - ((x>>1)&0x55); x = (x&0x33) + ((x>>2)&0x33); return static_cast<int>(((x + (x>>4)) & 0x0F)); };
    cw_len = std::clamp(cw_len, 5, 8);
    // Pack bits8[0..cw_len-1] msb-first into a byte
    uint8_t rx = 0;
    for (int i = 0; i < cw_len; ++i) rx = static_cast<uint8_t>((rx << 1) | (bits8[i] & 1u));
    int best_n = 0; int best_d = 9;
    bool use_cr5 = (cw_len == 5);
    for (int n = 0; n < 16; ++n) {
        uint8_t cw = use_cr5 ? LUT_CR5[n] : LUT[n];
        // Compare only cw_len MSBs
        uint8_t ref = static_cast<uint8_t>(cw >> (8 - cw_len));
        int d = popc8(static_cast<uint8_t>(ref ^ rx));
        if (d < best_d) { best_d = d; best_n = n; if (d == 0) break; }
    }
    if (distance) *distance = best_d;
    if (corrected) *corrected = (best_d != 0);
    // Return data nibble (MSB-first) from selected codeword (upper 4 bits)
    uint8_t cw_sel = ( (cw_len == 5) ? LUT_CR5[best_n] : LUT[best_n] );
    return static_cast<uint8_t>((cw_sel >> 4) & 0x0F);
}

} // namespace lora::standalone
