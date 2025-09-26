#include <iostream>
#include <fstream>
#include <vector>
#include <complex>
#include <string>
#include "frame_sync_lite.hpp"

using namespace std;
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
    
    // Use the legacy detect_preamble function
    auto result = detect_preamble(samples, 7, 4);
    
    if (result.frame_detected) {
        std::cout << "✓ Frame detected!"
                 << " CFO: " << result.frame_info.cfo_int << " + " << result.frame_info.cfo_frac
                 << ", SNR: " << result.snr_est << " dB" << std::endl;
    } else {
        std::cout << "✗ No frame detected\n";
    }
    
    return 0;
}
