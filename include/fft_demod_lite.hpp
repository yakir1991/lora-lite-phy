#pragma once
#include <vector>
#include <complex>
#include <cstdint>
#include <cmath>

namespace lora_lite {

struct DemodResult {
    uint16_t index = 0; // integer FFT bin with maximum magnitude
    float frac = 0.0f;  // parabolic-interpolated fractional bin offset in [-0.5, 0.5]
    double magnitude_sq = 0.0; // magnitude squared of peak bin
};

struct FftResult {
    uint16_t index = 0;
    double magnitude_sq = 0.0;
    std::vector<std::complex<float>> bins;
};

// Minimal hard-decision LoRa FFT demod replicating gr-lora-sdr logic for a single symbol stream.
// Given complex samples (one LoRa symbol = 2^sf samples) produces uint16_t symbol index (Gray not yet removed: matches fft_demod output after its internal rate adjustment & -1 shift handling).
class FftDemodLite {
public:
    explicit FftDemodLite(uint8_t sf) { set_sf(sf); }
    void set_sf(uint8_t sf) {
        m_sf = sf; m_N = 1u << m_sf; m_downchirp_base.resize(m_N); m_downchirp.resize(m_N); build_downchirp(); m_fft_mag.resize(m_N); m_fft_bins.resize(m_N);
    // Invalidate cached oversampled chirps
    m_os_cache.clear();
        // Reset CFO state
        m_cfo_int = 0; m_cfo_frac = 0.f;
    }
    uint8_t sf() const { return m_sf; }

    // Apply CFO (integer bins + fractional) to reference downchirp to mirror GR fft_demod behavior.
    // This rebuilds the working downchirp from the cached base (no CFO) and then applies
    // exp(-j 2π * (cfo_int + cfo_frac) * n / N) factor.
    void apply_cfo(int cfo_int, float cfo_frac){
        m_cfo_int = cfo_int; m_cfo_frac = cfo_frac;
        // Start from base (no CFO)
        m_downchirp = m_downchirp_base;
        float two_pi_over_N = 2.0f * 3.14159265358979323846f / (float)m_N;
        float cfo_total = (float)cfo_int + cfo_frac; // bins
        for(uint32_t n=0;n<m_N;n++){
            float ang = - two_pi_over_N * cfo_total * (float)n;
            float c = cosf(ang), s = sinf(ang);
            std::complex<float> rot(c, s); // exp(-j*ang)
            m_downchirp[n] *= rot;
        }
    }

    // Demod a single symbol (pointer must have N samples)
    uint16_t demod(const std::complex<float>* sym) {
        return demod_with_details(sym).index;
    }

    DemodResult demod_with_details(const std::complex<float>* sym) {
        // Dechirp & copy real/imag into temp arrays for naive DFT (N <= 4096 typical). For SF7 N=128.
        for (uint32_t i=0;i<m_N;i++) {
            auto d = sym[i] * m_downchirp[i];
            m_tmp_re[i] = d.real();
            m_tmp_im[i] = d.imag();
        }
        // Naive DFT magnitude^2 (can replace with kissfft / FFT later). For N=128 cost is fine.
        for (uint32_t k=0;k<m_N;k++) {
            float acc_re=0.f, acc_im=0.f;
            for (uint32_t n=0;n<m_N;n++) {
                float ang = -2.0f*3.14159265358979323846f*k*n / m_N;
                float c = cosf(ang); float s = sinf(ang);
                acc_re += m_tmp_re[n]*c - m_tmp_im[n]*s;
                acc_im += m_tmp_re[n]*s + m_tmp_im[n]*c;
            }
            m_fft_mag[k] = acc_re*acc_re + acc_im*acc_im;
            m_fft_bins[k] = std::complex<float>(acc_re, acc_im);
        }
        // Argmax
        uint32_t idx = 0; float best = m_fft_mag[0];
        for (uint32_t k=1;k<m_N;k++) if (m_fft_mag[k] > best){ best = m_fft_mag[k]; idx = k; }
        DemodResult res;
        res.index = static_cast<uint16_t>(idx);
        res.magnitude_sq = best;
        const uint32_t left_idx = (idx + m_N - 1) % m_N;
        const uint32_t right_idx = (idx + 1) % m_N;
        const float left = m_fft_mag[left_idx];
        const float right = m_fft_mag[right_idx];
        const float denom = (left - 2.0f * best + right);
        if (std::abs(denom) > 1e-12f) {
            float delta = 0.5f * (left - right) / denom;
            if (delta > 0.5f) delta = 0.5f;
            if (delta < -0.5f) delta = -0.5f;
            res.frac = delta;
        }
        return res;
    }

    FftResult demod_with_fft_details(const std::complex<float>* sym) {
        auto demod_res = demod_with_details(sym);
        FftResult result;
        result.index = demod_res.index;
        result.magnitude_sq = demod_res.magnitude_sq;
        result.bins.assign(m_fft_bins.begin(), m_fft_bins.end());
        return result;
    }
    
    FftResult demod_with_zero_padded_fft(const std::complex<float>* sym, uint32_t padding_factor) {
        // Dechirp first
        for (uint32_t i=0;i<m_N;i++) {
            auto d = sym[i] * m_downchirp[i];
            m_tmp_re[i] = d.real();
            m_tmp_im[i] = d.imag();
        }
        
        uint32_t padded_size = m_N * padding_factor;
        m_fft_bins_padded.resize(padded_size);
        std::vector<float> padded_mag(padded_size);
        
        // Zero-padded DFT
        for (uint32_t k=0;k<padded_size;k++) {
            float acc_re=0.f, acc_im=0.f;
            for (uint32_t n=0;n<m_N;n++) {  // Only sum over original N samples
                float ang = -2.0f*3.14159265358979323846f*k*n / padded_size;
                float c = cosf(ang); float s = sinf(ang);
                acc_re += m_tmp_re[n]*c - m_tmp_im[n]*s;
                acc_im += m_tmp_re[n]*s + m_tmp_im[n]*c;
            }
            padded_mag[k] = acc_re*acc_re + acc_im*acc_im;
            m_fft_bins_padded[k] = std::complex<float>(acc_re, acc_im);
        }
        
        // Find peak
        uint32_t idx = 0; 
        float best = padded_mag[0];
        for (uint32_t k=1;k<padded_size;k++) {
            if (padded_mag[k] > best) {
                best = padded_mag[k]; 
                idx = k;
            }
        }
        
        FftResult result;
        result.index = static_cast<uint16_t>(idx);
        result.magnitude_sq = best;
        result.bins = m_fft_bins_padded;
        return result;
    }
    
    const std::vector<std::complex<float>>& get_last_fft_bins() const {
        return m_fft_bins;
    }

    // Oversampled symbol demod (symbol has N*os samples). Strategy:
    // 1. Average (or sum) each group of 'os' consecutive samples to obtain length-N symbol.
    DemodResult demod_with_details_oversampled(const std::complex<float>* sym_os, uint8_t os){
        if(os<=1) return demod_with_details(sym_os);
        static thread_local std::vector<std::complex<float>> collapsed; // reuse
        collapsed.assign(m_N, {0.f,0.f});
        for(uint32_t k=0;k<m_N;k++){
            const std::complex<float>* g = sym_os + k*os;
            std::complex<float> acc{0.f,0.f};
            for(uint32_t j=0;j<os;j++) acc += g[j];
            collapsed[k] = acc;
        }
        return demod_with_details(collapsed.data());
    }
    uint16_t demod_oversampled(const std::complex<float>* sym_os, uint8_t os){
        return demod_with_details_oversampled(sym_os, os).index;
    }
private:
    uint8_t m_sf{}; uint32_t m_N{};
    std::vector<std::complex<float>> m_downchirp_base; // canonical downchirp (no CFO)
    std::vector<std::complex<float>> m_downchirp;       // possibly CFO-adjusted
    std::vector<float> m_fft_mag;
    std::vector<std::complex<float>> m_fft_bins;        // FFT bins from last demod
    std::vector<std::complex<float>> m_fft_bins_padded; // For zero-padded FFT
    // temp buffers sized to MAX 4096 (SF12) once (simple static arrays)
    float m_tmp_re[1u<<12]{}; float m_tmp_im[1u<<12]{};
    int m_cfo_int = 0; float m_cfo_frac = 0.f;
    void build_downchirp(){
        // downchirp is conj(upchirp(k,0)), formula similar to build_ref_chirps
        for(uint32_t n=0;n<m_N;n++) {
            float phase = -2.0f*3.14159265358979323846f*( ( (float)n*n /(2.0f*m_N) ) - 0.5f*n );
            m_downchirp_base[n] = std::complex<float>(cosf(phase), sinf(phase));
        }
        m_downchirp = m_downchirp_base; // initial (no CFO)
    }
    // Cache of oversampled downchirps keyed by os
    std::vector<std::pair<uint8_t, std::vector<std::complex<float>>>> m_os_cache;
    const std::vector<std::complex<float>>& get_downchirp_os(uint8_t os){
        for(auto& kv: m_os_cache) if(kv.first==os) return kv.second;
        // Build
        std::vector<std::complex<float>> v(m_N*os);
        // Formula with oversampling factor (matching reference chirp generation):
        // phase_up = 2π * ((n^2)/(2*N*os^2) - 0.5 * n / os)
        // downchirp = conj(up) => negative of phase_up
        double Nf = (double)m_N; double osf = (double)os;
        for(uint32_t n=0;n<m_N*os; ++n){
            // NOTE: Previously used os^2 in quadratic term denominator; corrected to os (time scaling) to match reference chirp discretization.
            double phase = -2.0 * 3.14159265358979323846 * ( ( (double)n * (double)n ) / (2.0 * Nf * osf) - 0.5 * (double)n / osf );
            v[n] = std::complex<float>(cos(phase), sin(phase));
        }
        m_os_cache.emplace_back(os, std::move(v));
        return m_os_cache.back().second;
    }
};

} // namespace lora_lite
