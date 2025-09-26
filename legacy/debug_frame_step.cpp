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
    
    // Test processing just one symbol worth at a time starting from the beginning
    size_t samples_per_symbol = (1 << sf) * 4;  // 4x oversampling
    std::cout << "Samples per symbol: " << samples_per_symbol << std::endl;
    
    for (int symb = 0; symb < 10 && symb * samples_per_symbol < numSamples; ++symb) {
        size_t start_idx = symb * samples_per_symbol;
        size_t end_idx = std::min(start_idx + samples_per_symbol, numSamples);
        size_t chunk_size = end_idx - start_idx;
        
        std::cout << "\n=== Processing symbol " << symb << " (samples " << start_idx << " to " << (end_idx-1) << ") ===\n";
        
        // Create input vector
        std::vector<std::complex<float>> input(samples.begin() + start_idx, samples.begin() + end_idx);
        std::cout << "Input size: " << input.size() << " samples\n";
        
        FrameSyncResult result = frameSync.process_samples(input);
        
        std::cout << "Result: consumed=" << result.samples_consumed 
                  << ", frame_detected=" << result.frame_detected
                  << ", symbol_ready=" << result.symbol_ready << std::endl;
        
        if (result.samples_consumed == 0) {
            std::cout << "ERROR: No samples consumed! Breaking..." << std::endl;
            break;
        }
    }
    
    return 0;
}
