#include <iostream>
#include <fstream>
#include <vector>
#include <complex>

int main() {
    std::string filename = "temp/hello_world.cf32";
    
    // Load exactly like test_enhanced_receiver.cpp
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open file " << filename << std::endl;
        return 1;
    }
    
    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // Read complex float samples
    size_t num_samples = file_size / sizeof(std::complex<float>);
    std::vector<std::complex<float>> samples(num_samples);
    
    file.read(reinterpret_cast<char*>(samples.data()), file_size);
    file.close();
    
    std::cout << "Loaded " << num_samples << " samples" << std::endl;
    std::cout << "File size: " << file_size << " bytes" << std::endl;
    std::cout << "Expected samples: " << file_size / 8 << std::endl;
    
    // Check sample at position 11484
    if (samples.size() > 11484) {
        std::cout << "Sample at 11484: (" << samples[11484].real() << "," << samples[11484].imag() << ")" << std::endl;
        std::cout << "Next 4 samples:" << std::endl;
        for (int i = 1; i <= 4; ++i) {
            if (11484 + i < samples.size()) {
                std::cout << "  [" << (11484 + i) << "]: (" << samples[11484 + i].real() << "," << samples[11484 + i].imag() << ")" << std::endl;
            }
        }
        
        std::cout << "\nExpected from Python:" << std::endl;
        std::cout << "  (-0.999699,0.0245407)" << std::endl;
        std::cout << "  (-0.992666,0.120888)" << std::endl;
        std::cout << "  (-0.977028,0.21311)" << std::endl;
        std::cout << "  (-0.953768,0.300544)" << std::endl;
        std::cout << "  (-0.923879,0.382684)" << std::endl;
    }
    
    return 0;
}
