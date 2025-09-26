#include <iostream>
#include <fstream>
#include <vector>
#include <complex>
#include "receiver_lite.hpp"

using namespace lora_lite;

int main(){
    // Load the hello_stupid_world vector  
    std::ifstream file("vectors/sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown", std::ios::binary);
    if (!file.is_open()) { 
        std::cerr << "Could not open file\n"; 
        return 1; 
    }
    
    std::vector<std::complex<float>> samples;
    std::complex<float> sample;
    while (file.read(reinterpret_cast<char*>(&sample), sizeof(sample))) {
        samples.push_back(sample);
    }
    file.close();
    
    std::cout << "Loaded " << samples.size() << " complex samples\n";
    
    // Test sync words that GNU Radio uses
    std::vector<std::pair<uint16_t, uint16_t>> sync_word_pairs = {
        {8, 16},     // GNU Radio modulated values for 0x12
        {18, 52},    // Raw 0x12, 0x34
    };
    
    // Test starting from high-energy regions we found  
    std::vector<size_t> start_positions = {15000, 16000, 16256, 16384, 55000, 55296, 55424};
    
    for (auto [sw1, sw2] : sync_word_pairs) {
        std::cout << "\n=== Testing sync words: " << sw1 << ", " << sw2 << " ===\n";
        
        for (size_t start_pos : start_positions) {
            if (start_pos >= samples.size()) continue;
            
            std::cout << "  Testing from position " << start_pos 
                     << " (" << (100.0 * start_pos / samples.size()) << "%)\n";
            
            // Set up receiver with these sync words
            RxParams params;
            params.sf = 7;
            params.cr = 2;
            params.has_crc = true;
            params.implicit_hdr = false;
            params.ldro = 0;
            params.oversample = 4;
            params.sync_words = {sw1, sw2};
            
            ReceiverLite receiver(params);
            
            // Process a chunk starting from this position
            size_t chunk_size = std::min(size_t(8192), samples.size() - start_pos);
            std::vector<std::complex<float>> chunk(samples.begin() + start_pos,
                                                  samples.begin() + start_pos + chunk_size);
            
            auto result = receiver.process_samples(chunk);
            
            if (result.frame_detected) {
                std::cout << "    ✓ FRAME DETECTED! CFO: " << result.cfo_int 
                         << " + " << result.cfo_frac << ", SNR: " << result.snr_est << " dB\n";
                std::cout << "      Payload bytes: " << result.payload.size() << "\n";
                if (!result.payload.empty()) {
                    std::cout << "      Hex: ";
                    for (uint8_t b : result.payload) {
                        printf("%02x ", b);
                    }
                    std::cout << "\n      Text: ";
                    for (uint8_t b : result.payload) {
                        if (b >= 32 && b < 127) std::cout << (char)b;
                        else std::cout << '.';
                    }
                    std::cout << "\n";
                }
                goto success; // Found a frame, exit
            }
        }
    }
    
    std::cout << "\n✗ No frames detected with any tested sync words or positions\n";
    return 1;
    
success:
    std::cout << "\n🎉 Successfully decoded the hello world vector!\n";
    return 0;
}
