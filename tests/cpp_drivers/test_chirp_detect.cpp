#include <iostream>
#include <vector>
#include <complex>
#include <fstream>
#include "include/fft_demod_lite.hpp"

using namespace lora_lite;

namespace {
constexpr double kPi = 3.14159265358979323846;

void build_ref_chirps(std::complex<float>* upchirp, std::complex<float>* downchirp, uint8_t sf) {
    const size_t N = 1u << sf;
    const double Nf = static_cast<double>(N);
    
    for (size_t n = 0; n < N; ++n) {
        const double nf = static_cast<double>(n);
        // Upchirp: phase = 2π * (n² / 2N - n/2)
        const double up_phase = 2.0 * kPi * (nf * nf / (2.0 * Nf) - nf / 2.0);
        upchirp[n] = std::complex<float>(std::cos(up_phase), std::sin(up_phase));
        // Downchirp is conjugate of upchirp
        downchirp[n] = std::conj(upchirp[n]);
    }
}

int get_symbol_val(const std::complex<float>* samples, const std::complex<float>* ref_chirp, uint8_t sf) {
    FftDemodLite demod(sf);
    const size_t N = 1u << sf;
    std::vector<std::complex<float>> dechirped(N);
    
    // Multiply with reference chirp
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
    
    size_t numSamples = fileSize / (2 * sizeof(float)); // Complex samples
    std::vector<std::complex<float>> samples(numSamples);
    file.read(reinterpret_cast<char*>(samples.data()), fileSize);
    file.close();
    
    std::cout << "Loaded " << numSamples << " IQ samples for SF" << sf << std::endl;
    std::cout << "Samples per symbol: " << samples_per_symbol << std::endl;
    
    // Build reference chirps
    std::vector<std::complex<float>> upchirp(N), downchirp(N);
    build_ref_chirps(upchirp.data(), downchirp.data(), sf);
    
    // Look through first several symbols to see if we find upchirps
    std::vector<std::complex<float>> downsampled(N);
    for (size_t symb = 0; symb < std::min(size_t(20), numSamples / samples_per_symbol); ++symb) {
        // Downsample the symbol (take every os_factor'th sample starting from offset os_factor/2)
        for (size_t i = 0; i < N; ++i) {
            size_t idx = symb * samples_per_symbol + os_factor / 2 + os_factor * i;
            if (idx < numSamples) {
                downsampled[i] = samples[idx];
            } else {
                downsampled[i] = std::complex<float>(0, 0);
            }
        }
        
        // Check if it matches upchirp pattern
        int val = get_symbol_val(downsampled.data(), downchirp.data(), sf);
        
        // Calculate power
        float power = 0;
        for (size_t i = 0; i < N; ++i) {
            power += std::norm(downsampled[i]);
        }
        power /= N;
        
        std::cout << "Symbol " << symb << ": val=" << val << ", power=" << power;
        
        // Additional FFT analysis
        FftDemodLite demod(sf);
        auto result = demod.demod_with_details(downsampled.data());
        std::cout << ", direct_fft_idx=" << result.index << ", magnitude_sq=" << result.magnitude_sq << std::endl;
        
        // Show top 3 bins
        auto fft_result = demod.demod_with_fft_details(downsampled.data());
        std::vector<std::pair<float, int>> mag_idx;
        for (int i = 0; i < N; ++i) {
            mag_idx.push_back({std::norm(fft_result.bins[i]), i});
        }
        std::sort(mag_idx.rbegin(), mag_idx.rend());
        
        std::cout << "  Top bins: (" << mag_idx[0].second << ":" << mag_idx[0].first << ") ";
        std::cout << "(" << mag_idx[1].second << ":" << mag_idx[1].first << ") ";
        std::cout << "(" << mag_idx[2].second << ":" << mag_idx[2].first << ")" << std::endl;
    }
    
    return 0;
}
