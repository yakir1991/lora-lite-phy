#include "enhanced_receiver_lite.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <complex>
#include <chrono>

using namespace lora_lite;

std::vector<std::complex<float>> load_cf32_file(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open file " << filename << std::endl;
        return {};
    }
    
    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // Read complex float samples
    size_t num_samples = file_size / sizeof(std::complex<float>);
    std::vector<std::complex<float>> samples(num_samples);
    
    file.read(reinterpret_cast<char*>(samples.data()), file_size);
    file.close();
    
    std::cout << "Loaded " << num_samples << " samples from " << filename << std::endl;
    return samples;
}

void print_results(const EnhancedRxResult& result) {
    std::cout << "\n📊 ENHANCED RECEIVER RESULTS:" << std::endl;
    std::cout << "=================================" << std::endl;
    std::cout << "Status: " << (result.ok ? "SUCCESS ✅" : "FAILED ❌") << std::endl;
    std::cout << "Frame Detected: " << (result.frame_detected ? "YES" : "NO") << std::endl;
    
    if (result.frame_detected) {
        std::cout << "Frame Position: " << result.frame_position << std::endl;
        std::cout << "Detection Confidence: " << (result.detection_confidence * 100.0f) << "%" << std::endl;
        std::cout << "Detection Method: " << result.detection_method << std::endl;
        
        if (!result.raw_symbols.empty()) {
            std::cout << "Raw Symbols: [";
            for (size_t i = 0; i < result.raw_symbols.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.raw_symbols[i];
            }
            std::cout << "]" << std::endl;
            
            std::cout << "Gray Decoded: [";
            for (size_t i = 0; i < result.gray_decoded.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.gray_decoded[i];
            }
            std::cout << "]" << std::endl;
        }
        
        if (!result.method_performance.empty()) {
            std::cout << "Method Performance:" << std::endl;
            for (const auto& entry : result.method_performance) {
                std::cout << "  " << entry.first << ": " << (entry.second * 100.0f) << "%" << std::endl;
            }
        }
    }
    
    std::cout << "=================================" << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "🚀 ENHANCED LORA RECEIVER V3 C++ TEST" << std::endl;
    std::cout << "=====================================" << std::endl;
    
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <input_file.cf32>" << std::endl;
        return 1;
    }
    
    std::string filename = argv[1];
    
    // Load samples
    auto samples = load_cf32_file(filename);
    if (samples.empty()) {
        std::cerr << "Error: No samples loaded" << std::endl;
        return 1;
    }
    
    // Debug: Check specific sample at position 11484
    std::cout << "C++ sample at 11484: (" << samples[11484].real() << "," << samples[11484].imag() << ")" << std::endl;
    std::cout << "C++ samples around 11484: ";
    for (int i = 0; i < 5; ++i) {
        std::cout << "(" << samples[11484+i].real() << "," << samples[11484+i].imag() << ") ";
    }
    std::cout << std::endl;
    
    // Setup enhanced receiver parameters
    EnhancedRxParams params;
    params.sf = 7;           // SF7
    params.cr = 2;           // CR=4/6
    params.has_crc = true;   // CRC enabled
    params.implicit_hdr = false; // Explicit header
    params.ldro = 0;         // LDRO off
    params.oversample = 4;   // 4x oversampling
    params.sync_words = {0x12, 0x34}; // Standard sync words
    
    // V3 Enhanced parameters
    params.enable_enhanced_detection = true;
    params.enable_adaptive_learning = true;
    params.enable_enhanced_preprocessing = false;  // DISABLE preprocessing for V3 compatibility
    params.correlation_threshold = 0.5f;
    params.validation_threshold = 0.4f;
    
    std::cout << "🔧 Enhanced Receiver Configuration:" << std::endl;
    std::cout << "   SF=" << (int)params.sf << ", CR=" << (int)params.cr 
              << ", CRC=" << (params.has_crc ? "ON" : "OFF") << std::endl;
    std::cout << "   Enhanced Detection: " << (params.enable_enhanced_detection ? "ON" : "OFF") << std::endl;
    std::cout << "   Adaptive Learning: " << (params.enable_adaptive_learning ? "ON" : "OFF") << std::endl;
    std::cout << "   Enhanced Preprocessing: " << (params.enable_enhanced_preprocessing ? "ON" : "OFF") << std::endl;
    
    // Create enhanced receiver
    EnhancedReceiverLite receiver(params);
    
    // Process samples
    std::cout << "\n🔍 Processing samples..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();
    
    auto result = receiver.process_samples(samples);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Print results
    print_results(result);
    
    std::cout << "⏱️  Processing Time: " << duration.count() << " ms" << std::endl;
    
    // Compare with expected results for known vectors
    if (filename.find("hello_world") != std::string::npos || filename.find("hello_stupid_world") != std::string::npos) {
        std::vector<uint16_t> expected_symbols = {9, 1, 53, 0, 20, 4, 72, 12};
        
        if (result.raw_symbols.size() == expected_symbols.size()) {
            bool perfect_match = true;
            for (size_t i = 0; i < expected_symbols.size(); ++i) {
                if (result.raw_symbols[i] != expected_symbols[i]) {
                    perfect_match = false;
                    break;
                }
            }
            
            if (perfect_match) {
                std::cout << "🎉 PERFECT MATCH with expected symbols!" << std::endl;
            } else {
                std::cout << "⚠️  Different from expected symbols: [";
                for (size_t i = 0; i < expected_symbols.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << expected_symbols[i];
                }
                std::cout << "]" << std::endl;
            }
        }
    }
    
    return result.ok ? 0 : 1;
}
