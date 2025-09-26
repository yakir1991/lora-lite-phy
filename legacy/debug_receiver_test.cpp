
#include <iostream>
#include <vector>
#include <complex>
#include <fstream>
#include "include/receiver_lite.hpp"

using namespace lora_lite;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <cf32_file>" << std::endl;
        return 1;
    }

    // Load samples
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<std::complex<float>> samples;
    
    float real, imag;
    while (file.read(reinterpret_cast<char*>(&real), sizeof(float)) &&
           file.read(reinterpret_cast<char*>(&imag), sizeof(float))) {
        samples.push_back(std::complex<float>(real, imag));
    }
    
    std::cout << "הוטענו " << samples.size() << " דגימות" << std::endl;
    
    // Setup receiver
    ReceiverParams params;
    params.sf = 7;
    params.oversample = 4;
    params.sync_words = {93, 112};  // הערכים שמצאנו
    
    ReceiverLite receiver(params);
    
    // Process in chunks
    const size_t chunk_size = 1024;
    std::vector<uint8_t> payload;
    
    for (size_t i = 0; i < samples.size(); i += chunk_size) {
        size_t end = std::min(i + chunk_size, samples.size());
        std::vector<std::complex<float>> chunk(samples.begin() + i, samples.begin() + end);
        
        auto result = receiver.process_samples(chunk);
        
        if (result.frame_detected) {
            std::cout << "🎯 Frame detected at sample " << i << std::endl;
        }
        
        if (!result.payload.empty()) {
            std::cout << "📦 Payload received (" << result.payload.size() << " bytes): ";
            for (auto byte : result.payload) {
                std::cout << std::hex << (int)byte << " ";
            }
            std::cout << std::endl;
            
            // Try to decode as ASCII
            std::string ascii_payload;
            for (auto byte : result.payload) {
                if (byte >= 32 && byte <= 126) {
                    ascii_payload += (char)byte;
                } else {
                    ascii_payload += '.';
                }
            }
            std::cout << "📜 ASCII: '" << ascii_payload << "'" << std::endl;
            
            payload = result.payload;
            break;
        }
    }
    
    if (payload.empty()) {
        std::cout << "❌ לא נמצא payload" << std::endl;
    }
    
    return 0;
}
