#pragma once
#include <vector>
#include <cstdint>
#include <complex>
#include <optional>
#include "fft_demod_lite.hpp"
#include "frame_sync_lite.hpp"
#include "gray.hpp"
#include <vector>
#include "interleaver.hpp"
#include "hamming.hpp"
#include "whitening.hpp"
#include "crc16.hpp"
#include "bit_packing.hpp"

namespace lora_lite {

struct RxParams {
    uint8_t sf;        // spreading factor
    uint8_t cr;        // coding rate (1..4)
    bool has_crc;      // payload contains CRC16
    bool implicit_hdr; // implicit header mode
    uint8_t ldro;      // ldro mode (0/1/2)
    uint8_t oversample = 4; // oversampling factor
    std::array<uint16_t, 2> sync_words = {0x12, 0x34}; // network sync words
};

struct RxResult {
    bool ok = false;           // crc + structural success
    bool crc_ok = false;       // CRC validity (if has_crc)
    bool frame_detected = false; // frame sync detected
    std::vector<uint8_t> payload; // dewhitened payload bytes (without CRC)
    float snr_est = 0.0f;      // SNR estimate from frame sync
    int cfo_int = 0;           // integer CFO estimate
    float cfo_frac = 0.0f;     // fractional CFO estimate
};

// End-to-end LoRa receiver with frame synchronization and decoding:
// - Processes raw IQ samples to detect frames and decode payload
// - Integrates frame sync, demod, error correction, and CRC validation
class ReceiverLite {
public:
    explicit ReceiverLite(const RxParams& p);
    
    // Process raw IQ samples - returns result when frame is complete
    RxResult process_samples(const std::vector<std::complex<float>>& samples);
    
    // Legacy interface: decode pre-synchronized symbols (bypasses frame sync)
    void apply_cfo(int cfo_int, float cfo_frac);
    RxResult decode(const std::complex<float>* samples, size_t symbol_count);
    
    // Reset receiver state
    void reset();
    
    // Debug accessors
    const std::vector<uint8_t>& last_post_hamming_bits() const { return m_last_post_hamming_bits; }
    const std::vector<uint8_t>& last_header_codewords_raw() const { return m_last_header_codewords_raw; }
    const std::vector<uint8_t>& last_header_nibbles_raw() const { return m_last_header_nibbles_raw; }
    const std::vector<uint16_t>& last_syms_proc() const { return m_last_syms_proc; }
    const std::vector<uint16_t>& last_degray() const { return m_last_degray; }
private:
    RxParams m_p; 
    FftDemodLite m_fft; 
    FrameSyncLite m_frame_sync;
    HammingTables m_tables;
    
    // Receiver state
    enum class State {
        SYNC,           // Frame synchronization
        DECODE_HEADER,  // Decoding header symbols
        DECODE_PAYLOAD  // Decoding payload symbols
    } m_state = State::SYNC;
    
    size_t m_symbols_received = 0;
    size_t m_symbols_needed = 0;
    std::vector<std::complex<float>> m_symbol_buffer;
    
    // Debug state
    std::vector<uint8_t> m_last_post_hamming_bits;
    std::vector<uint8_t> m_last_header_codewords_raw;
    std::vector<uint8_t> m_last_header_nibbles_raw;
    std::vector<uint16_t> m_last_syms_proc;
    std::vector<uint16_t> m_last_degray;
    
    // Chosen header variant parameters (for debug)
    int m_last_header_offset = 0; 
    int m_last_header_divide_then_gray = 0; 
    int m_last_header_norm_mode = 1; 
    int m_last_header_lin_a = 0; 
    int m_last_header_lin_b = 0; 
    int m_last_header_variant_score = -1; 
    uint16_t m_last_crc_calc = 0;
    uint8_t m_last_crc_expected_lsb = 0;
    uint8_t m_last_crc_expected_msb = 0;
    uint8_t m_last_crc_observed_lsb = 0;
    uint8_t m_last_crc_observed_msb = 0;
    size_t m_last_crc_payload_len = 0;
    static CodeRate map_cr(uint8_t cr){ switch(cr){ case 1: return CodeRate::CR45; case 2: return CodeRate::CR46; case 3: return CodeRate::CR47; default: return CodeRate::CR48; } }
public:
    int last_header_offset() const { return m_last_header_offset; }
    int last_header_divide_then_gray() const { return m_last_header_divide_then_gray; }
    int last_header_norm_mode() const { return m_last_header_norm_mode; }
    int last_header_linear_a() const { return m_last_header_lin_a; }
    int last_header_linear_b() const { return m_last_header_lin_b; }
    int last_header_variant_score() const { return m_last_header_variant_score; }
    uint16_t last_crc_calc() const { return m_last_crc_calc; }
    uint8_t last_crc_expected_lsb() const { return m_last_crc_expected_lsb; }
    uint8_t last_crc_expected_msb() const { return m_last_crc_expected_msb; }
    uint8_t last_crc_observed_lsb() const { return m_last_crc_observed_lsb; }
    uint8_t last_crc_observed_msb() const { return m_last_crc_observed_msb; }
    size_t last_crc_payload_len() const { return m_last_crc_payload_len; }
    std::vector<uint16_t> m_last_degray;
        // Chosen header variant parameters (for debug)
        int m_last_header_offset = 0; // integer offset applied after normalization
        int m_last_header_divide_then_gray = 0; // 1 if divide-by-4 then gray decode
        int m_last_header_norm_mode = 1; // 0=raw,1=idx-1,2=idx+1
        int m_last_header_lin_a = 0; // per-symbol linear coefficient
        int m_last_header_lin_b = 0; // base offset b
        int m_last_header_variant_score = -1; // aggregate score (sum minimal distances)
        uint16_t m_last_crc_calc = 0;
        uint8_t m_last_crc_expected_lsb = 0;
        uint8_t m_last_crc_expected_msb = 0;
        uint8_t m_last_crc_observed_lsb = 0;
        uint8_t m_last_crc_observed_msb = 0;
        size_t m_last_crc_payload_len = 0;
    static CodeRate map_cr(uint8_t cr){ switch(cr){ case 1: return CodeRate::CR45; case 2: return CodeRate::CR46; case 3: return CodeRate::CR47; default: return CodeRate::CR48; } }
public:
    int last_header_offset() const { return m_last_header_offset; }
    int last_header_divide_then_gray() const { return m_last_header_divide_then_gray; }
    int last_header_norm_mode() const { return m_last_header_norm_mode; }
    int last_header_linear_a() const { return m_last_header_lin_a; }
    int last_header_linear_b() const { return m_last_header_lin_b; }
        int last_header_variant_score() const { return m_last_header_variant_score; }
        uint16_t last_crc_calc() const { return m_last_crc_calc; }
        uint8_t last_crc_expected_lsb() const { return m_last_crc_expected_lsb; }
        uint8_t last_crc_expected_msb() const { return m_last_crc_expected_msb; }
        uint8_t last_crc_observed_lsb() const { return m_last_crc_observed_lsb; }
        uint8_t last_crc_observed_msb() const { return m_last_crc_observed_msb; }
        size_t last_crc_payload_len() const { return m_last_crc_payload_len; }
};

} // namespace lora_lite
