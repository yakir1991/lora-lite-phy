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
    
    // Create frame sync with detailed debug
    FrameSyncLite frameSync(sf);
    
    // Process samples in chunks, showing detailed state
    size_t chunkSize = 1 << sf; // One symbol worth
    size_t totalProcessed = 0;
    
    for (size_t i = 0; i < numSamples; i += chunkSize) {
        size_t actualChunkSize = std::min(chunkSize, numSamples - i);
        
        // Convert to vector for API
        std::vector<std::complex<float>> chunk(samples.data() + i, samples.data() + i + actualChunkSize);
        
        FrameSyncResult result = frameSync.process_samples(chunk);
        
        std::cout << "Chunk " << (i / chunkSize) << " (samples " << i << "-" << (i + actualChunkSize - 1) << "):" << std::endl;
        std::cout << "  consumed=" << result.samples_consumed << std::endl;
        std::cout << "  frame_detected=" << result.frame_detected << std::endl; 
        std::cout << "  symbol_ready=" << result.symbol_ready << std::endl;
        std::cout << "  snr_est=" << result.snr_est << std::endl;
        std::cout << "  sfo_hat=" << result.sfo_hat << std::endl;
        
        // Show some signal statistics
        std::complex<float> sum(0,0);
        float power = 0;
        for (size_t j = 0; j < actualChunkSize; ++j) {
            sum += samples[i + j];
            power += std::norm(samples[i + j]);
        }
        float avgPower = power / actualChunkSize;
        std::cout << "  avg_power=" << avgPower << std::endl;
        std::cout << "  avg_real=" << sum.real() / actualChunkSize << std::endl;
        std::cout << "  avg_imag=" << sum.imag() / actualChunkSize << std::endl;
        
        totalProcessed += result.samples_consumed;
        
        if (result.frame_detected) {
            std::cout << "*** FRAME DETECTED! ***" << std::endl;
            std::cout << "  cfo_int=" << result.frame_info.cfo_int << std::endl;
            std::cout << "  cfo_frac=" << result.frame_info.cfo_frac << std::endl;
            std::cout << "  sf=" << (int)result.frame_info.sf << std::endl;
            std::cout << "  cr=" << (int)result.frame_info.cr << std::endl;
            std::cout << "  pay_len=" << (int)result.frame_info.pay_len << std::endl;
            std::cout << "  has_crc=" << result.frame_info.has_crc << std::endl;
            break;
        }
    }
    
    std::cout << "Processed " << totalProcessed << " samples total" << std::endl;
    
    return 0;
}
