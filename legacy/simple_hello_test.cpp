#include <iostream>
#include <fstream>
#include <vector>
#include <complex>

using namespace std;

// Simple function to test different sync word combinations
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
    
    // Look for preamble patterns in the data
    // LoRa preamble consists of upchirps, so let's look for frequency sweeps
    
    // Calculate magnitude for each sample
    std::vector<float> magnitudes;
    for (const auto& s : samples) {
        magnitudes.push_back(std::abs(s));
    }
    
    // Find strong signal regions (where preamble likely is)
    float avg_mag = 0;
    for (float mag : magnitudes) avg_mag += mag;
    avg_mag /= magnitudes.size();
    
    float threshold = avg_mag * 2;
    std::cout << "Average magnitude: " << avg_mag << ", threshold: " << threshold << std::endl;
    
    std::vector<size_t> strong_regions;
    for (size_t i = 0; i < magnitudes.size(); i++) {
        if (magnitudes[i] > threshold) {
            strong_regions.push_back(i);
        }
    }
    
    std::cout << "Found " << strong_regions.size() << " samples above threshold\n";
    
    if (!strong_regions.empty()) {
        std::cout << "Strong signal starts around sample " << strong_regions[0] 
                 << " (" << (100.0 * strong_regions[0] / samples.size()) << "%)\n";
    }
    
    return 0;
}
