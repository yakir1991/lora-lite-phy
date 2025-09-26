#include <iostream>
#include <fstream>
#include <vector>
#include <complex>
#include "frame_sync_lite.hpp"
#include "fft_demod_lite.hpp"

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
    
    // Create frame sync with different sync word combinations to test
    std::vector<std::pair<uint16_t, uint16_t>> sync_word_pairs = {
        {18, 52},    // GNU Radio default (0x12, 0x34)
        {71, 87},    // Our original raw values
        {93, 112},   // Our CFO-corrected values
        {68, 85},    // Alternative values
        {33, 66}     // LoRa public network sync words
    };
    
    for (auto [sw1, sw2] : sync_word_pairs) {
        std::cout << "\n=== Testing sync words: " << sw1 << ", " << sw2 << " ===\n";
        
        FrameSyncLite frame_sync(7, 4, {sw1, sw2});
        
        size_t processed = 0;
        const size_t chunk_size = 512;
        
        while (processed < samples.size()) {
            size_t remaining = samples.size() - processed;
            size_t to_process = std::min(chunk_size, remaining);
            
            std::vector<std::complex<float>> chunk(samples.begin() + processed, 
                                                  samples.begin() + processed + to_process);
            
            auto result = frame_sync.process_samples(chunk);
            
            if (result.frame_detected) {
                std::cout << "✓ Frame detected at sample " << processed 
                         << "! CFO: " << result.frame_info.cfo_int << " + " << result.frame_info.cfo_frac
                         << ", SNR: " << result.snr_est << " dB, symbols: " << result.symbol_out.size() << std::endl;
                break;
            }
            
            processed += to_process;
            
            // Show progress every 4096 samples
            if (processed % 4096 == 0 && processed > 0) {
                std::cout << "  Processed " << processed << "/" << samples.size() 
                         << " samples (" << (100 * processed / samples.size()) << "%)" << std::endl;
            }
        }
        
        if (processed >= samples.size()) {
            std::cout << "✗ No frame detected with these sync words\n";
        }
    }
    
    return 0;
}
