#include <iostream>
#include <fstream>
#include <vector>
#include <complex>
#include <cstring>
#include <algorithm>
#include <limits>
#include <cmath>
#include "include/frame_sync_lite.hpp"

using namespace lora_lite;

int main(int argc, char** argv){
    if(argc < 2){ std::cerr << "Usage: run_vector_direct <file> [sf] [cr] [crc(0/1)] [implicit(0/1)] [ldro]\n"; return 1; }
    const char* path = argv[1];
    uint8_t sf = (argc>2)? (uint8_t)std::atoi(argv[2]):7;
    
    std::ifstream f(path, std::ios::binary); 
    if(!f){ 
        std::cerr << "Cannot open file: " << path << "\n"; 
        return 1; 
    }
    
    std::vector<float> buf((std::istreambuf_iterator<char>(f)), {});
    if(buf.empty()){ 
        std::cerr<<"Empty file or read error\n"; 
        return 1; 
    }
    
    size_t byte_count = buf.size();
    if(byte_count % sizeof(float)!=0){ 
        std::cerr<<"File size not multiple of 4 bytes\n"; 
    }
    
    size_t float_count = byte_count / sizeof(float);
    float* fptr = reinterpret_cast<float*>(buf.data());
    if(float_count % 2){ 
        std::cerr<<"Float count not even (I/Q mismatch)\n"; 
        return 1; 
    }
    
    size_t complex_count = float_count / 2;
    std::vector<std::complex<float>> raw(complex_count);
    for(size_t i=0; i<complex_count; i++) {
        raw[i] = { fptr[2*i], fptr[2*i+1] };
    }
    
    std::cout << "Loaded " << complex_count << " IQ samples" << std::endl;
    
    // Create frame sync directly with the correct sync words
    std::array<uint16_t, 2> sync_words = {71, 87};
    FrameSyncLite frameSync(sf, 4, sync_words);
    std::cout << "Using sync words: " << sync_words[0] << ", " << sync_words[1] << std::endl;
    
    // Process as a continuous stream (match working logic exactly)
    size_t total_consumed = 0;
    int iteration = 0;
    bool frame_detected = false;
    int symbols_received = 0;
    
    while (total_consumed < raw.size() && iteration < 20) {  // Same limit as working test
        size_t remaining = raw.size() - total_consumed;
        size_t chunk_size = std::min(remaining, static_cast<size_t>(1 << sf) * 4);
        
        std::vector<std::complex<float>> input(raw.begin() + total_consumed, 
                                               raw.begin() + total_consumed + chunk_size);
        
        FrameSyncResult result = frameSync.process_samples(input);
        
        std::cout << "Iteration " << iteration << ": input_size=" << input.size() 
                  << ", consumed=" << result.samples_consumed
                  << ", frame_detected=" << result.frame_detected 
                  << ", symbol_ready=" << result.symbol_ready;
        
        if (result.frame_detected && !frame_detected) {
            frame_detected = true;
            std::cout << " *** FRAME DETECTED! ***";
            std::cout << " cfo_int=" << result.frame_info.cfo_int
                      << ", cfo_frac=" << result.frame_info.cfo_frac
                      << ", sf=" << (int)result.frame_info.sf;
        }
        
        if (result.symbol_ready) {
            symbols_received++;
            std::cout << " [Symbol " << symbols_received << " ready]";
        }
        
        std::cout << std::endl;
        
        if (result.samples_consumed == 0) {
            std::cout << "ERROR: No samples consumed! Breaking..." << std::endl;
            break;
        }
        
        total_consumed += result.samples_consumed;
        iteration++;
        
        if (frame_detected) {
            std::cout << "Frame detected! Continuing to process payload symbols..." << std::endl;
        }
    }
    
    std::cout << "Final status: frame_detected=" << frame_detected 
              << ", symbols_received=" << symbols_received 
              << ", total_consumed=" << total_consumed << std::endl;
    
    return frame_detected ? 0 : 1;
}
