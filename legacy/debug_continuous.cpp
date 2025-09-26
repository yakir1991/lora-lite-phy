#include <iostream>
#include <vector>
#include <complex>
#include <fstream>
#include "include/frame_sync_lite.hpp"

using namespace lora_lite;

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <cf32_file> <sf>" << std::endl;
        return 1;
    }

    std::string filename = argv[1];
    int sf = std::stoi(argv[2]);
    
    // Read the CF32 file
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Error opening file: " << filename << std::endl;
        return 1;
    }
    
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    size_t numSamples = fileSize / (2 * sizeof(float)); // Complex samples
    std::vector<std::complex<float>> samples(numSamples);
    file.read(reinterpret_cast<char*>(samples.data()), fileSize);
    file.close();
    
    std::cout << "Loaded " << numSamples << " IQ samples for SF" << sf << std::endl;
    
    // Create frame sync with default params
    FrameSyncLite frameSync(sf);
    
    // Process as a continuous stream
    size_t total_consumed = 0;
    int iteration = 0;
    
    while (total_consumed < numSamples && iteration < 50) {  // Safety limit
        size_t remaining = numSamples - total_consumed;
        size_t chunk_size = std::min(remaining, static_cast<size_t>(1 << sf) * 4);  // One symbol
        
        std::vector<std::complex<float>> input(samples.begin() + total_consumed, 
                                               samples.begin() + total_consumed + chunk_size);
        
        FrameSyncResult result = frameSync.process_samples(input);
        
        std::cout << "Iteration " << iteration << ": input_size=" << input.size() 
                  << ", consumed=" << result.samples_consumed
                  << ", frame_detected=" << result.frame_detected 
                  << ", symbol_ready=" << result.symbol_ready;
        
        if (result.frame_detected) {
            std::cout << " *** FRAME DETECTED! ***";
            std::cout << " cfo_int=" << result.frame_info.cfo_int
                      << ", cfo_frac=" << result.frame_info.cfo_frac
                      << ", sf=" << (int)result.frame_info.sf
                      << ", cr=" << (int)result.frame_info.cr;
        }
        
        std::cout << std::endl;
        
        if (result.samples_consumed == 0) {
            std::cout << "ERROR: No samples consumed! Breaking..." << std::endl;
            break;
        }
        
        total_consumed += result.samples_consumed;
        iteration++;
        
        if (result.frame_detected) {
            std::cout << "Frame detected, continuing to process symbols..." << std::endl;
            // Continue processing to get the payload symbols
        }
    }
    
    std::cout << "Total consumed: " << total_consumed << " samples" << std::endl;
    
    return 0;
}
