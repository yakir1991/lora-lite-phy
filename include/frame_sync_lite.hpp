#pragma once
#include <cstddef>
#include <cstdint>
#include <complex>
#include <vector>
#include <array>

namespace lora_lite {

enum class FrameSyncState {
    DETECT,
    SYNC,
    SFO_COMPENSATION
};

enum class NetIdState {
    NET_ID1,
    NET_ID2,
    DOWNCHIRP1,
    DOWNCHIRP2,
    QUARTER_DOWN
};

struct FrameInfo {
    bool is_header = true;
    int cfo_int = 0;
    float cfo_frac = 0.0f;
    uint8_t sf = 7;
    uint8_t cr = 1;
    uint8_t pay_len = 0;
    bool has_crc = false;
    bool ldro = false;
    uint32_t symb_numb = 0;
    bool invalid_header = false;
};

struct FrameSyncResult {
    bool frame_detected = false;
    bool symbol_ready = false;
    size_t samples_consumed = 0;
    std::vector<std::complex<float>> symbol_out;
    FrameInfo frame_info;
    
    // Sync diagnostics
    float snr_est = 0.0f;
    float sfo_hat = 0.0f;
    bool off_by_one_id = false;
};

class FrameSyncLite {
private:
    // Configuration
    uint8_t m_sf = 7;
    uint32_t m_number_of_bins = 128;
    uint8_t m_os_factor = 4;
    uint32_t m_samples_per_symbol = 512;
    std::array<uint16_t, 2> m_sync_words = {0x12, 0x34}; // Default sync words
    uint16_t m_preamb_len = 8;
    uint32_t m_n_up_req = 5; // preamb_len - 3
    uint32_t up_symb_to_use = 4; // m_n_up_req - 1
    
    // State machine
    FrameSyncState m_state = FrameSyncState::DETECT;
    NetIdState net_id_state = NetIdState::NET_ID1;
    
    // Detection state
    int bin_idx = -1;
    int bin_idx_new = 0;
    int symbol_cnt = 1;
    int k_hat = 0;
    std::vector<int> preamb_up_vals;
    int additional_upchirps = 0;
    
    // Sync state
    float m_cfo_frac = 0.0f;
    float m_sto_frac = 0.0f;
    int m_cfo_int = 0;
    float sfo_hat = 0.0f;
    float sfo_cum = 0.0f;
    std::array<int, 2> net_ids = {0, 0};
    int net_id_off = 0;
    int one_symbol_off = 0;
    int down_val = 0;
    
    // Frame tracking
    bool m_received_head = false;
    uint32_t m_symb_numb = 0;
    uint32_t frame_cnt = 0;
    
    // Working buffers
    std::vector<std::complex<float>> m_upchirp;
    std::vector<std::complex<float>> m_downchirp;
    std::vector<std::complex<float>> preamble_raw;
    std::vector<std::complex<float>> preamble_upchirps;
    std::vector<std::complex<float>> CFO_frac_correc;
    std::vector<std::complex<float>> symb_corr;
    std::vector<std::complex<float>> in_down;
    std::vector<std::complex<float>> net_id_samp;
    std::vector<std::complex<float>> additional_symbol_samp;
    
    // Helper methods
    void build_ref_chirps();
    int get_symbol_val(const std::complex<float>* samples, const std::complex<float>* ref_chirp);
    float estimate_CFO_frac_Bernier(const std::complex<float>* samples);
    float estimate_STO_frac();
    int most_frequent(const int* values, size_t len);
    float determine_snr(const std::complex<float>* samples);
    
public:
    FrameSyncLite(uint8_t sf = 7, uint8_t os_factor = 4, 
                  std::array<uint16_t, 2> sync_words = {0x12, 0x34},
                  uint16_t preamble_len = 8);
    
    FrameSyncResult process_samples(const std::vector<std::complex<float>>& input);
    void handle_frame_info(const FrameInfo& frame_info);
    void reset();
};

// Legacy function for backward compatibility
FrameSyncResult detect_preamble(const std::vector<std::complex<float>>& raw,
                                uint8_t sf,
                                uint8_t oversample);

} // namespace lora_lite
