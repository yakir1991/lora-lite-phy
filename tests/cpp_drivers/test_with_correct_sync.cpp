#include <iostream>
#include <vector>
#include <complex>
#include <fstream>
#include <cmath>
#include "include/frame_sync_lite.hpp"
#include "include/fft_demod_lite.hpp"

using namespace lora_lite;

// Simple symbol demodulator using FftDemodLite
int demodulate_symbol(const std::vector<std::complex<float>>& samples, int sf) {
    if (samples.empty()) return -1;
    
    FftDemodLite demod(sf);
    int N = 1 << sf;  // 2^sf
    
    // If we have more samples than N, decimate
    if (samples.size() > N) {
        std::vector<std::complex<float>> decimated(N);
        int step = samples.size() / N;
        for (int i = 0; i < N; i++) {
            decimated[i] = samples[i * step];
        }
        return demod.demod(decimated.data());
    } else if (samples.size() == N) {
        return demod.demod(samples.data());
    } else {
        // Pad with zeros
        std::vector<std::complex<float>> padded(N, std::complex<float>(0, 0));
        for (size_t i = 0; i < samples.size(); i++) {
            padded[i] = samples[i];
        }
        return demod.demod(padded.data());
    }
}

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
    
    size_t numSamples = fileSize / (2 * sizeof(float));
    std::vector<std::complex<float>> samples(numSamples);
    file.read(reinterpret_cast<char*>(samples.data()), fileSize);
    file.close();
    
    std::cout << "Loaded " << numSamples << " IQ samples for SF" << sf << std::endl;
    
    // Create frame sync with the CFO-corrected sync words: 93, 112
    std::array<uint16_t, 2> sync_words = {93, 112};
    FrameSyncLite frameSync(sf, 4, sync_words);
    
    // Process as a continuous stream
    size_t total_consumed = 0;
    int iteration = 0;
    
    std::cout << "Using sync words: " << sync_words[0] << ", " << sync_words[1] << std::endl;
    
    while (total_consumed < numSamples && iteration < 20) {  // Reduced limit for easier reading
        size_t remaining = numSamples - total_consumed;
        size_t chunk_size = std::min(remaining, static_cast<size_t>(1 << sf) * 4);
        
        std::vector<std::complex<float>> input(samples.begin() + total_consumed, 
                                               samples.begin() + total_consumed + chunk_size);
        
        FrameSyncResult result = frameSync.process_samples(input);
        
        std::cout << "Iteration " << iteration << ": consumed=" << result.samples_consumed
                  << ", frame_detected=" << result.frame_detected 
                  << ", symbol_ready=" << result.symbol_ready
                  << ", snr_est=" << result.snr_est;
        
        if (result.frame_detected) {
            std::cout << " *** FRAME DETECTED! ***";
            std::cout << " cfo_int=" << result.frame_info.cfo_int
                      << ", cfo_frac=" << result.frame_info.cfo_frac
                      << ", sf=" << (int)result.frame_info.sf;
        }
        
        if (result.symbol_ready) {
            std::cout << " SYMBOL_READY(size=" << result.symbol_out.size() << ")";
            if (!result.symbol_out.empty()) {
                // Demodulate the symbol
                int symbol_value = demodulate_symbol(result.symbol_out, 7);
                std::cout << " symbol=" << symbol_value;
                
                // Also show first few raw samples
                std::cout << " [";
                for (size_t i = 0; i < std::min(size_t(2), result.symbol_out.size()); i++) {
                    std::cout << std::abs(result.symbol_out[i]) << " ";
                }
                if (result.symbol_out.size() > 2) std::cout << "...";
                std::cout << "]";
            } else {
                std::cout << " (empty symbol_out)";
            }
        }
        
        // Add state debugging
        std::cout << " consumed=" << result.samples_consumed << " total=" << total_consumed;
        
        std::cout << std::endl;
        
        if (result.samples_consumed == 0) {
            std::cout << "ERROR: No samples consumed! Breaking..." << std::endl;
            break;
        }
        
        total_consumed += result.samples_consumed;
        iteration++;
        
        if (result.frame_detected) {
            std::cout << "Frame detected! Continuing to process payload symbols..." << std::endl;
        }
        
        if (result.symbol_ready) {
            std::cout << "Symbol ready! symbol_out size: " << result.symbol_out.size() << std::endl;
            if (!result.symbol_out.empty()) {
                // Convert complex symbol to LoRa symbol value
                // Symbol is the phase/frequency representation, need to do FFT to get symbol value
                std::vector<std::complex<float>>& symbol = result.symbol_out;
                
                // Print raw symbol data
                std::cout << "Raw symbol (" << symbol.size() << " samples): ";
                if (symbol.size() <= 8) {
                    for (const auto& s : symbol) {
                        std::cout << "(" << s.real() << "+" << s.imag() << "j) ";
                    }
                } else {
                    std::cout << "first 4: ";
                    for (int i = 0; i < 4 && i < symbol.size(); i++) {
                        std::cout << "(" << symbol[i].real() << "+" << symbol[i].imag() << "j) ";
                    }
                    std::cout << " ... last 4: ";
                    for (int i = std::max(0, (int)symbol.size()-4); i < symbol.size(); i++) {
                        std::cout << "(" << symbol[i].real() << "+" << symbol[i].imag() << "j) ";
                    }
                }
                std::cout << std::endl;
                
                // Try to compute symbol value using FFT if we have enough samples
                if (symbol.size() == 128) {  // SF7 = 2^7 = 128
                    // Simple FFT to find peak
                    // For simplicity, let's just find the maximum magnitude
                    int max_idx = 0;
                    float max_mag = 0;
                    for (int i = 0; i < symbol.size(); i++) {
                        float mag = std::abs(symbol[i]);
                        if (mag > max_mag) {
                            max_mag = mag;
                            max_idx = i;
                        }
                    }
                    std::cout << "Symbol value estimate: " << max_idx << " (max magnitude: " << max_mag << ")" << std::endl;
                }
            }
        }
    }
    
    std::cout << "Total consumed: " << total_consumed << " samples" << std::endl;
    
    return 0;
}
