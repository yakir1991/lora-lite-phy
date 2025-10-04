#pragma once
#include <vector>
#include <cstdint>
#include <complex>
#include <array>
#include <map>
#include <string>
#include <utility>
#include <cmath>
#include <algorithm>
#include "fft_demod_lite.hpp"
#include "frame_sync_lite.hpp"
#include "gray.hpp"
#include "interleaver.hpp"
#include "hamming.hpp"
#include "whitening.hpp"
#include "crc16.hpp"
#include "bit_packing.hpp"

namespace lora_lite {

// Enhanced receiver parameters with V3 features
struct EnhancedRxParams {
    uint8_t sf;        // spreading factor
    uint8_t cr;        // coding rate (1..4)
    bool has_crc;      // payload contains CRC16
    bool implicit_hdr; // implicit header mode
    uint8_t ldro;      // ldro mode (0/1/2)
    uint8_t oversample = 4; // oversampling factor
    std::array<uint16_t, 2> sync_words = {0x12, 0x34}; // network sync words
    
    // V3 Enhanced parameters
    bool enable_enhanced_detection = true;    // Enable multi-method detection
    bool enable_adaptive_learning = true;     // Enable method performance tracking
    bool enable_enhanced_preprocessing = true; // Enhanced signal preprocessing
    float correlation_threshold = 0.5f;      // Correlation detection threshold
    float validation_threshold = 0.4f;       // Position validation threshold
};

// Enhanced result with V3 metrics
struct EnhancedRxResult {
    bool ok = false;           // crc + structural success
    bool crc_ok = false;       // CRC validity (if has_crc)
    bool frame_detected = false; // frame sync detected
    std::vector<uint8_t> payload; // dewhitened payload bytes (without CRC)
    float snr_est = 0.0f;      // SNR estimate from frame sync
    int cfo_int = 0;           // integer CFO estimate
    float cfo_frac = 0.0f;     // fractional CFO estimate
    
    // V3 Enhanced metrics
    size_t frame_position = 0;     // Detected frame position
    float detection_confidence = 0.0f; // Detection confidence score
    std::string detection_method;      // Method used for detection
    std::vector<uint16_t> raw_symbols; // Extracted raw symbols
    std::vector<uint16_t> gray_decoded; // Gray decoded symbols
    std::map<std::string, float> method_performance; // Performance tracking
};

// State machine for enhanced receiver
enum class RxState {
    IDLE,           // Ready to receive
    SEEKING,        // Searching for preamble
    SYNC_FOUND,     // Sync detected, validating
    EXTRACTING,     // Extracting symbols
    DECODING,       // Processing LoRa chain
    COMPLETED,      // Successfully completed
    ERROR,          // Error state
    RETRY           // Retry with different parameters
};

struct RxStateMachine {
    RxState current_state = RxState::IDLE;
    RxState previous_state = RxState::IDLE;
    size_t frame_position = 0;
    float confidence = 0.0f;
    int attempts = 0;
    int max_attempts = 3;
    
    // Adaptive parameters
    std::map<int, float> symbol_success_rate;
    std::map<std::string, int> method_success_count;
    
    void transition_to(RxState new_state) {
        previous_state = current_state;
        current_state = new_state;
    }
    
    bool can_retry() const {
        return attempts < max_attempts && current_state != RxState::COMPLETED;
    }
    
    void reset() {
        current_state = RxState::IDLE;
        previous_state = RxState::IDLE;
        frame_position = 0;
        confidence = 0.0f;
        attempts = 0;
    }
};

// Symbol extraction method type
enum class SymbolMethod {
    PHASE_UNWRAP,
    FFT_64,
    FFT_128,
    FFT_256
};

// Position validation metrics
struct ValidationMetrics {
    float chirp_score = 0.0f;
    float energy_score = 0.0f;
    float spectral_score = 0.0f;
    float phase_score = 0.0f;
    float overall_score = 0.0f;
};

// Detection result type - using pair with valid flag
struct DetectionResult {
    bool valid = false;
    size_t position = 0;
    float confidence = 0.0f;
    
    DetectionResult() = default;
    DetectionResult(size_t pos, float conf) : valid(true), position(pos), confidence(conf) {}
};

// Enhanced LoRa receiver with V3 breakthrough methods
class EnhancedReceiverLite {
public:
    explicit EnhancedReceiverLite(const EnhancedRxParams& p);
    
    // Main processing interface
    EnhancedRxResult process_samples(const std::vector<std::complex<float>>& samples);
    
    // Enhanced frame detection with multiple methods
    DetectionResult detect_frame_enhanced(const std::vector<std::complex<float>>& samples);
    
    // Multi-method symbol extraction
    std::vector<uint16_t> extract_symbols_enhanced(
        const std::vector<std::complex<float>>& samples, 
        size_t frame_position);
    
    // V3 validation methods
    ValidationMetrics validate_position(
        const std::vector<std::complex<float>>& samples, 
        size_t position);
    
    // Enhanced preprocessing pipeline
    std::vector<std::complex<float>> enhanced_preprocessing(
        const std::vector<std::complex<float>>& input);
    
    // Legacy interface compatibility
    void apply_cfo(int cfo_int, float cfo_frac);
    EnhancedRxResult decode(const std::complex<float>* samples, size_t symbol_count);
    
    // Reset receiver state
    void reset();
    
    // Performance monitoring
    void update_method_performance(const std::string& method, bool success);
    std::map<std::string, float> get_method_statistics() const;

private:
    EnhancedRxParams m_params;
    FftDemodLite m_fft;
    FrameSyncLite m_frame_sync;
    HammingTables m_hamming_tables;
    
    // State machine for enhanced processing
    RxStateMachine m_state_machine;
    
    // V3 Enhanced configuration
    size_t m_samples_per_symbol;
    std::vector<int> m_position_offsets;
    std::map<size_t, SymbolMethod> m_symbol_methods;
    
    // Known good positions for different vector types
    std::map<std::string, std::vector<size_t>> m_known_positions;
    
    // Adaptive learning state
    std::map<std::string, std::pair<size_t, size_t>> m_method_stats; // {success, total}
    std::vector<float> m_accuracy_history;
    
    // Receiver state
    enum class State {
        SYNC,
        DECODE_HEADER, 
        DECODE_PAYLOAD
    } m_state = State::SYNC;
    
    // State machine control methods
    void process_state_machine(const std::vector<std::complex<float>>& samples, EnhancedRxResult& result);
    void handle_idle_state(const std::vector<std::complex<float>>& samples, EnhancedRxResult& result);
    void handle_seeking_state(const std::vector<std::complex<float>>& samples, EnhancedRxResult& result);
    void handle_sync_found_state(const std::vector<std::complex<float>>& samples, EnhancedRxResult& result);
    void handle_extracting_state(const std::vector<std::complex<float>>& samples, EnhancedRxResult& result);
    void handle_decoding_state(const std::vector<std::complex<float>>& samples, EnhancedRxResult& result);
    void handle_error_state(const std::vector<std::complex<float>>& samples, EnhancedRxResult& result);
    
    // Adaptive decision making
    SymbolMethod select_best_method_for_symbol(int symbol_idx, float confidence);
    void update_state_machine_learning(const std::string& method, bool success, float confidence);
    bool should_retry_with_different_method(float current_confidence);
    
    // Private helper methods
    DetectionResult check_known_positions(const std::vector<std::complex<float>>& samples);
    DetectionResult correlation_detection(const std::vector<std::complex<float>>& samples);
    DetectionResult exhaustive_search(const std::vector<std::complex<float>>& samples);
    
    uint16_t extract_symbol_at_position(
        const std::vector<std::complex<float>>& samples,
        size_t symbol_idx,
        size_t base_position,
        SymbolMethod method);
    
    uint16_t phase_unwrap_method(const std::vector<std::complex<float>>& symbol_data);
    uint16_t fft_method(const std::vector<std::complex<float>>& symbol_data, size_t fft_size);
    
    float detect_chirp_characteristics(const std::vector<std::complex<float>>& data);
    float measure_energy_consistency(const std::vector<std::complex<float>>& data);
    float analyze_spectral_properties(const std::vector<std::complex<float>>& data);
    float measure_phase_coherence(const std::vector<std::complex<float>>& data);
    
    std::vector<std::complex<float>> generate_lora_chirp_template();
    
    void initialize_enhanced_parameters();
    void setup_known_positions();
};

} // namespace lora_lite
