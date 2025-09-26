#include <iostream>
#include <vector>
#include <complex>
#include <fstream>
#include <iomanip>
#include "include/frame_sync_lite.hpp"
#include "include/fft_demod_lite.hpp"

using namespace lora_lite;

namespace {
constexpr double kPi = 3.14159265358979323846;

void build_ref_chirps(std::complex<float>* upchirp, std::complex<float>* downchirp, uint8_t sf) {
    const size_t N = 1u << sf;
    const double Nf = static_cast<double>(N);
    
    for (size_t n = 0; n < N; ++n) {
        const double nf = static_cast<double>(n);
        const double up_phase = 2.0 * kPi * (nf * nf / (2.0 * Nf) - nf / 2.0);
        upchirp[n] = std::complex<float>(std::cos(up_phase), std::sin(up_phase));
        downchirp[n] = std::conj(upchirp[n]);
    }
}

int get_symbol_val(const std::complex<float>* samples, const std::complex<float>* ref_chirp, uint8_t sf) {
    FftDemodLite demod(sf);
    const size_t N = 1u << sf;
    std::vector<std::complex<float>> dechirped(N);
    
    for (uint32_t i = 0; i < N; i++) {
        dechirped[i] = samples[i] * ref_chirp[i];
    }
    
    auto result = demod.demod_with_details(dechirped.data());
    return result.magnitude_sq > 1e-6 ? result.index : -1;
}

}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <cf32_file> <sf>" << std::endl;
        return 1;
    }

    std::string filename = argv[1];
    int sf = std::stoi(argv[2]);
    const size_t N = 1u << sf;
    const size_t os_factor = 4;
    const size_t samples_per_symbol = N * os_factor;
    
    // Read the CF32 file
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Error opening file: " << filename << std::endl;
        return 1;
    }
    
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    size_t numSamples = fileSize / (2 * sizeof(float));
    std::vector<std::complex<float>> samples(numSamples);
    file.read(reinterpret_cast<char*>(samples.data()), fileSize);
    file.close();
    
    std::cout << "Loaded " << numSamples << " IQ samples for SF" << sf << std::endl;
    
    // Build reference chirps
    std::vector<std::complex<float>> upchirp(N), downchirp(N);
    build_ref_chirps(upchirp.data(), downchirp.data(), sf);
    
    // Manually analyze the frame structure
    std::vector<std::complex<float>> downsampled(N);
    
    std::cout << "\nExpected LoRa frame structure analysis:\n";
    for (size_t symb = 0; symb < 20 && symb * samples_per_symbol < numSamples; ++symb) {
        // Downsample the symbol
        for (size_t i = 0; i < N; ++i) {
            size_t idx = symb * samples_per_symbol + os_factor / 2 + os_factor * i;
            if (idx < numSamples) {
                downsampled[i] = samples[idx];
            } else {
                downsampled[i] = std::complex<float>(0, 0);
            }
        }
        
        // Check if it matches upchirp pattern (using downchirp ref)
        int upchirp_val = get_symbol_val(downsampled.data(), downchirp.data(), sf);
        
        // Check if it matches downchirp pattern (using upchirp ref) 
        int downchirp_val = get_symbol_val(downsampled.data(), upchirp.data(), sf);
        
        std::string symbol_type;
        if (symb < 8) {
            symbol_type = "preamble";
        } else if (symb < 10) {
            symbol_type = "network_id";
        } else if (symb < 12) {
            symbol_type = "downchirp";
        } else if (symb < 17) {  // 5 header symbols
            symbol_type = "header";
        } else {
            symbol_type = "payload";
        }
        
        std::cout << "Symbol " << std::setw(2) << symb << " (" << std::setw(10) << symbol_type << "): "
                  << "upchirp_val=" << std::setw(3) << upchirp_val 
                  << ", downchirp_val=" << std::setw(3) << downchirp_val << std::endl;
    }
    
    return 0;
}
