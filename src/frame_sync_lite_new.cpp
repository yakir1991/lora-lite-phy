#include "frame_sync_lite.hpp"
#include "fft_demod_lite.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <iostream>
#include <cstring>

namespace lora_lite {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float LDRO_MAX_DURATION_MS = 16.0f;

int mod(int a, int b) {
    return ((a % b) + b) % b;
}

void build_ref_chirps(std::complex<float>* upchirp, std::complex<float>* downchirp, uint8_t sf) {
    const size_t N = 1u << sf;
    const double Nf = static_cast<double>(N);
    
    for (size_t n = 0; n < N; ++n) {
        const double nf = static_cast<double>(n);
        // Upchirp: phase = 2π * (n² / 2N - n/2)
        const double up_phase = 2.0 * kPi * (nf * nf / (2.0 * Nf) - nf / 2.0);
        upchirp[n] = std::complex<float>(std::cos(up_phase), std::sin(up_phase));
        // Downchirp is conjugate of upchirp
        downchirp[n] = std::conj(upchirp[n]);
    }
}

} // namespace

FrameSyncLite::FrameSyncLite(uint8_t sf, uint8_t os_factor, 
                             std::array<uint16_t, 2> sync_words,
                             uint16_t preamble_len) {
    m_sf = sf;
    m_number_of_bins = 1u << sf;
    m_os_factor = os_factor;
    m_samples_per_symbol = m_number_of_bins * os_factor;
    m_sync_words = sync_words;
    m_preamb_len = preamble_len;
    
    if (preamble_len < 5) {
        std::cerr << "Preamble length should be greater than 5!" << std::endl;
    }
    
    m_n_up_req = preamble_len - 3;
    up_symb_to_use = m_n_up_req - 1;
    
    // Convert sync word if needed
    if (m_sync_words[0] != 0 && m_sync_words[1] == 0) {
        uint16_t tmp = m_sync_words[0];
        m_sync_words[0] = ((tmp & 0xF0) >> 4) << 3;
        m_sync_words[1] = (tmp & 0x0F) << 3;
    }
    
    // Initialize buffers
    m_upchirp.resize(m_number_of_bins);
    m_downchirp.resize(m_number_of_bins);
    preamble_raw.resize(m_preamb_len * m_number_of_bins);
    preamble_upchirps.resize(m_preamb_len * m_number_of_bins);
    CFO_frac_correc.resize(m_number_of_bins);
    symb_corr.resize(m_number_of_bins);
    in_down.resize(m_number_of_bins);
    net_id_samp.resize(static_cast<size_t>(m_samples_per_symbol * 2.5));
    additional_symbol_samp.resize(2 * m_samples_per_symbol);
    preamb_up_vals.resize(m_n_up_req, 0);
    
    build_ref_chirps();
}

void FrameSyncLite::build_ref_chirps() {
    ::lora_lite::build_ref_chirps(m_upchirp.data(), m_downchirp.data(), m_sf);
}

int FrameSyncLite::get_symbol_val(const std::complex<float>* samples, const std::complex<float>* ref_chirp) {
    FftDemodLite demod(m_sf);
    std::vector<std::complex<float>> dechirped(m_number_of_bins);
    
    // Multiply with reference chirp
    for (uint32_t i = 0; i < m_number_of_bins; i++) {
        dechirped[i] = samples[i] * ref_chirp[i];
    }
    
    auto result = demod.demod_with_details(dechirped.data());
    return result.index;
}

float FrameSyncLite::estimate_CFO_frac_Bernier(const std::complex<float>* samples) {
    FftDemodLite demod(m_sf);
    std::vector<int> k0(up_symb_to_use);
    std::vector<double> k0_mag(up_symb_to_use);
    std::vector<std::complex<double>> fft_val(up_symb_to_use * m_number_of_bins);
    
    for (int i = 0; i < up_symb_to_use; i++) {
        std::vector<std::complex<float>> dechirped(m_number_of_bins);
        const std::complex<float>* sym_samples = &samples[m_number_of_bins * i];
        
        // Dechirp with downchirp
        for (uint32_t j = 0; j < m_number_of_bins; j++) {
            dechirped[j] = sym_samples[j] * m_downchirp[j];
        }
        
        // Get FFT result
        auto fft_result = demod.demod_with_fft_details(dechirped.data());
        k0[i] = fft_result.index;
        k0_mag[i] = fft_result.magnitude_sq;
        
        // Store FFT values for phase calculation
        for (uint32_t j = 0; j < m_number_of_bins; j++) {
            fft_val[j + i * m_number_of_bins] = static_cast<std::complex<double>>(fft_result.bins[j]);
        }
    }
    
    // Get argmax
    int idx_max = k0[std::distance(k0_mag.begin(), std::max_element(k0_mag.begin(), k0_mag.end()))];
    
    // Fourth-order cumulant method (Bernier)
    std::complex<double> four_cum(0.0, 0.0);
    for (int i = 0; i < up_symb_to_use - 1; i++) {
        four_cum += fft_val[idx_max + m_number_of_bins * i] * 
                   std::conj(fft_val[idx_max + m_number_of_bins * (i + 1)]);
    }
    
    float cfo_frac = -std::arg(four_cum) / (2.0f * kPi);
    
    // Apply CFO correction to preamble
    for (uint32_t n = 0; n < up_symb_to_use * m_number_of_bins; n++) {
        std::complex<float> correction = std::polar(1.0f, static_cast<float>(-2.0f * kPi * cfo_frac / m_number_of_bins * n));
        preamble_upchirps[n] = samples[n] * correction;
    }
    
    return cfo_frac;
}

float FrameSyncLite::estimate_STO_frac() {
    FftDemodLite demod(m_sf);
    std::vector<float> fft_mag_sq(2 * m_number_of_bins, 0.0f);
    
    for (int i = 0; i < up_symb_to_use; i++) {
        std::vector<std::complex<float>> dechirped(m_number_of_bins);
        const std::complex<float>* sym_samples = &preamble_upchirps[m_number_of_bins * i];
        
        // Dechirp with downchirp
        for (uint32_t j = 0; j < m_number_of_bins; j++) {
            dechirped[j] = sym_samples[j] * m_downchirp[j];
        }
        
        // Zero-padded FFT (2x)
        auto fft_result = demod.demod_with_zero_padded_fft(dechirped.data(), 2);
        
        // Accumulate magnitude squared
        for (uint32_t j = 0; j < 2 * m_number_of_bins; j++) {
            fft_mag_sq[j] += std::norm(fft_result.bins[j]);
        }
    }
    
    // Find peak
    int k0 = std::distance(fft_mag_sq.begin(), std::max_element(fft_mag_sq.begin(), fft_mag_sq.end()));
    
    // RCTSL estimation
    double Y_1 = fft_mag_sq[mod(k0 - 1, 2 * m_number_of_bins)];
    double Y0 = fft_mag_sq[k0];
    double Y1 = fft_mag_sq[mod(k0 + 1, 2 * m_number_of_bins)];
    
    // Constants from Cui Yang (eq.15)
    double u = 64.0 * m_number_of_bins / 406.5506497;
    double v = u * 2.4674;
    
    // RCTSL
    double wa = (Y1 - Y_1) / (u * (Y1 + Y_1) + v * Y0);
    double ka = wa * m_number_of_bins / kPi;
    double k_residual = std::fmod((k0 + ka) / 2.0, 1.0);
    float sto_frac = k_residual - (k_residual > 0.5 ? 1.0f : 0.0f);
    
    return sto_frac;
}

int FrameSyncLite::most_frequent(const int* values, size_t len) {
    if (len == 0) return 0;
    
    std::vector<int> hist(m_number_of_bins, 0);
    for (size_t i = 0; i < len; i++) {
        int idx = mod(values[i], m_number_of_bins);
        hist[idx]++;
    }
    
    return std::distance(hist.begin(), std::max_element(hist.begin(), hist.end()));
}

float FrameSyncLite::determine_snr(const std::complex<float>* samples) {
    FftDemodLite demod(m_sf);
    std::vector<std::complex<float>> dechirped(m_number_of_bins);
    
    // Dechirp with downchirp
    for (uint32_t i = 0; i < m_number_of_bins; i++) {
        dechirped[i] = samples[i] * m_downchirp[i];
    }
    
    auto fft_result = demod.demod_with_fft_details(dechirped.data());
    
    double total_energy = 0.0;
    for (uint32_t i = 0; i < m_number_of_bins; i++) {
        total_energy += std::norm(fft_result.bins[i]);
    }
    
    double signal_energy = fft_result.magnitude_sq;
    double noise_energy = total_energy - signal_energy;
    
    return 10.0f * std::log10(signal_energy / std::max(noise_energy, 1e-12));
}

FrameSyncResult FrameSyncLite::process_samples(const std::vector<std::complex<float>>& input) {
    FrameSyncResult result;
    result.samples_consumed = 0;
    
    if (input.size() < m_samples_per_symbol) {
        return result;
    }
    
    // Downsample input
    for (uint32_t i = 0; i < m_number_of_bins; i++) {
        size_t idx = m_os_factor / 2 + m_os_factor * i - 
                    static_cast<size_t>(std::round(m_sto_frac * m_os_factor));
        if (idx < input.size()) {
            in_down[i] = input[idx];
        }
    }
    
    switch (m_state) {
        case FrameSyncState::DETECT: {
            bin_idx_new = get_symbol_val(in_down.data(), m_downchirp.data());
            
            // Look for consecutive reference upchirps (with margin of ±1)
            if (std::abs(mod(std::abs(bin_idx_new - bin_idx) + 1, m_number_of_bins) - 1) <= 1 && 
                bin_idx_new != -1) {
                if (symbol_cnt == 1 && bin_idx != -1) {
                    preamb_up_vals[0] = bin_idx;
                }
                
                if (symbol_cnt < static_cast<int>(preamb_up_vals.size())) {
                    preamb_up_vals[symbol_cnt] = bin_idx_new;
                }
                
                // Store preamble data
                size_t copy_idx = std::min(static_cast<size_t>(symbol_cnt), 
                                         preamble_raw.size() / m_number_of_bins - 1);
                std::memcpy(&preamble_raw[m_number_of_bins * copy_idx], 
                           in_down.data(), m_number_of_bins * sizeof(std::complex<float>));
                
                symbol_cnt++;
            } else {
                // Reset and start new potential preamble
                std::memcpy(&preamble_raw[0], in_down.data(), 
                           m_number_of_bins * sizeof(std::complex<float>));
                symbol_cnt = 1;
            }
            
            bin_idx = bin_idx_new;
            
            if (symbol_cnt == static_cast<int>(m_n_up_req)) {
                additional_upchirps = 0;
                m_state = FrameSyncState::SYNC;
                net_id_state = NetIdState::NET_ID1;
                symbol_cnt = 0;
                
                k_hat = most_frequent(preamb_up_vals.data(), preamb_up_vals.size());
                
                // Coarse synchronization
                result.samples_consumed = m_os_factor * (m_number_of_bins - k_hat);
            } else {
                result.samples_consumed = m_samples_per_symbol;
            }
            break;
        }
        
        case FrameSyncState::SYNC: {
            result.samples_consumed = m_samples_per_symbol;
            
            // Estimate CFO and STO on first entry
            if (symbol_cnt == 0) {
                m_cfo_frac = estimate_CFO_frac_Bernier(&preamble_raw[m_number_of_bins - k_hat]);
                m_sto_frac = estimate_STO_frac();
                
                // Create CFO correction vector
                for (uint32_t n = 0; n < m_number_of_bins; n++) {
                    CFO_frac_correc[n] = std::polar(1.0f, static_cast<float>(-2.0f * kPi * m_cfo_frac / m_number_of_bins * n));
                }
            }
            
            // Apply CFO correction
            for (uint32_t i = 0; i < m_number_of_bins; i++) {
                symb_corr[i] = in_down[i] * CFO_frac_correc[i];
            }
            
            bin_idx = get_symbol_val(symb_corr.data(), m_downchirp.data());
            
            switch (net_id_state) {
                case NetIdState::NET_ID1: {
                    if (bin_idx == 0 || bin_idx == 1 || 
                        static_cast<uint32_t>(bin_idx) == m_number_of_bins - 1) {
                        // Additional upchirp detected
                        additional_upchirps++;
                    } else {
                        net_ids[0] = bin_idx;
                        net_id_state = NetIdState::NET_ID2;
                    }
                    break;
                }
                
                case NetIdState::NET_ID2: {
                    net_ids[1] = bin_idx;
                    net_id_state = NetIdState::DOWNCHIRP1;
                    break;
                }
                
                case NetIdState::DOWNCHIRP1: {
                    net_id_state = NetIdState::DOWNCHIRP2;
                    break;
                }
                
                case NetIdState::DOWNCHIRP2: {
                    down_val = get_symbol_val(symb_corr.data(), m_upchirp.data());
                    net_id_state = NetIdState::QUARTER_DOWN;
                    break;
                }
                
                case NetIdState::QUARTER_DOWN: {
                    // Calculate integer CFO from downchirp
                    if (static_cast<uint32_t>(down_val) < m_number_of_bins / 2) {
                        m_cfo_int = static_cast<int>(std::floor(down_val / 2.0));
                    } else {
                        m_cfo_int = static_cast<int>(std::floor((down_val - static_cast<int>(m_number_of_bins)) / 2.0));
                    }
                    
                    // Validate network IDs
                    bool valid_frame = true;
                    net_id_off = 0;
                    
                    if (m_sync_words[0] != 0) {
                        net_id_off = net_ids[0] - static_cast<int>(m_sync_words[0]);
                        if (std::abs(net_id_off) > 2) {
                            valid_frame = false;
                        }
                    }
                    
                    if (valid_frame && m_sync_words[1] != 0) {
                        int net_id2_corrected = mod(net_ids[1] - net_id_off, m_number_of_bins);
                        if (net_id2_corrected != static_cast<int>(m_sync_words[1])) {
                            valid_frame = false;
                        }
                    }
                    
                    if (valid_frame) {
                        // Frame detected - transition to compensation
                        m_state = FrameSyncState::SFO_COMPENSATION;
                        frame_cnt++;
                        symbol_cnt = 0;
                        m_received_head = false;
                        
                        // Emit frame info
                        result.frame_detected = true;
                        result.frame_info.is_header = true;
                        result.frame_info.cfo_int = m_cfo_int;
                        result.frame_info.cfo_frac = m_cfo_frac;
                        result.frame_info.sf = m_sf;
                        
                        // Calculate SNR estimate
                        result.snr_est = determine_snr(&preamble_upchirps[0]);
                        
                        // Adjust consumption for timing correction
                        result.samples_consumed += m_samples_per_symbol / 4 + m_os_factor * m_cfo_int;
                        result.samples_consumed -= m_os_factor * net_id_off;
                    } else {
                        // Invalid frame - return to detection
                        reset();
                    }
                    break;
                }
            }
            
            symbol_cnt++;
            break;
        }
        
        case FrameSyncState::SFO_COMPENSATION: {
            // Output symbols for header/payload processing
            if (symbol_cnt < 8 || (symbol_cnt < static_cast<int>(m_symb_numb) && m_received_head)) {
                result.symbol_ready = true;
                result.symbol_out.assign(in_down.begin(), in_down.end());
                result.samples_consumed = m_samples_per_symbol;
                
                // SFO compensation (simplified)
                sfo_cum += sfo_hat;
                if (std::abs(sfo_cum) > 1.0f / (2.0f * m_os_factor)) {
                    int correction = (sfo_cum > 0) ? 1 : -1;
                    result.samples_consumed -= correction;
                    sfo_cum -= correction / static_cast<float>(m_os_factor);
                }
                
                symbol_cnt++;
            } else if (!m_received_head) {
                // Wait for header decoding
                result.samples_consumed = 0;
            } else {
                // Frame complete - return to detection
                reset();
                result.samples_consumed = m_samples_per_symbol;
            }
            break;
        }
    }
    
    return result;
}

void FrameSyncLite::handle_frame_info(const FrameInfo& frame_info) {
    if (frame_info.invalid_header) {
        reset();
    } else {
        // Calculate number of payload symbols
        bool ldro = frame_info.ldro;
        if (ldro == false) { // AUTO mode
            ldro = (static_cast<float>(1u << m_sf) * 1e3 / 125000.0f) > LDRO_MAX_DURATION_MS;
        }
        
        m_symb_numb = 8 + static_cast<uint32_t>(std::ceil(
            (2.0 * frame_info.pay_len - m_sf + 2.0 + 5.0 + (frame_info.has_crc ? 4.0 : 0.0)) /
            (m_sf - 2.0 * (ldro ? 1.0 : 0.0))
        )) * (4 + frame_info.cr);
        
        m_received_head = true;
    }
}

void FrameSyncLite::reset() {
    m_state = FrameSyncState::DETECT;
    symbol_cnt = 1;
    k_hat = 0;
    m_sto_frac = 0.0f;
    m_received_head = false;
    m_symb_numb = 0;
}

// Legacy function for backward compatibility
FrameSyncResult detect_preamble(const std::vector<std::complex<float>>& raw,
                                uint8_t sf,
                                uint8_t oversample) {
    FrameSyncLite sync(sf, oversample);
    return sync.process_samples(raw);
}

} // namespace lora_lite
