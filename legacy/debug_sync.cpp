#include <iostream>
#include <fstream>
#include <vector>
#include <complex>
#include "frame_sync_lite.hpp"

using namespace lora_lite;

int main(int argc, char** argv){
    if(argc < 2){ std::cerr << "Usage: debug_sync <file> [sf]\n"; return 1; }
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
    
    size_t float_count = buf.size() / sizeof(float);
    float* fptr = reinterpret_cast<float*>(buf.data());
    size_t complex_count = float_count / 2;
    std::vector<std::complex<float>> raw(complex_count);
    for(size_t i=0; i<complex_count; i++) {
        raw[i] = { fptr[2*i], fptr[2*i+1] };
    }
    
    std::cout << "Loaded " << complex_count << " IQ samples for SF" << static_cast<int>(sf) << std::endl;
    
    // Create frame sync with debug output
    FrameSyncLite sync(sf, 4, {0x12, 0x34}, 8);
    
    // Process in chunks to see what happens
    const size_t chunk_size = (1u << sf) * 4 * 16; // 16 symbols at a time
    size_t processed = 0;
    
    while (processed < raw.size() && processed < chunk_size * 10) { // Limit to first 10 chunks for debug
        size_t remaining = raw.size() - processed;
        size_t to_process = std::min(chunk_size, remaining);
        
        std::vector<std::complex<float>> chunk(raw.begin() + processed, 
                                              raw.begin() + processed + to_process);
        
        auto result = sync.process_samples(chunk);
        
        std::cout << "Chunk " << (processed / chunk_size) 
                  << ": consumed=" << result.samples_consumed
                  << ", frame_detected=" << result.frame_detected
                  << ", symbol_ready=" << result.symbol_ready << std::endl;
        
        if (result.frame_detected) {
            std::cout << "*** FRAME DETECTED! ***" << std::endl;
            std::cout << "CFO: " << result.frame_info.cfo_int << " + " << result.frame_info.cfo_frac << std::endl;
            std::cout << "SNR: " << result.snr_est << " dB" << std::endl;
            break;
        }
        
        processed += chunk_size; // Move by full chunk regardless of consumption
    }
    
    std::cout << "Processed " << processed << " samples total" << std::endl;
    return 0;
}
