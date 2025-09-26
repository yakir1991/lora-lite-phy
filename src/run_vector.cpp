#include <iostream>
#include <fstream>
#include <vector>
#include <complex>
#include <cstring>
#include <algorithm>
#include <limits>
#include <cmath>
#include "receiver_lite.hpp"

using namespace lora_lite;

int main(int argc, char** argv){
    if(argc < 2){ std::cerr << "Usage: run_vector <file> [sf] [cr] [crc(0/1)] [implicit(0/1)] [ldro] [sync_word1] [sync_word2]\n"; return 1; }
    const char* path = argv[1];
    uint8_t sf = (argc>2)? (uint8_t)std::atoi(argv[2]):7;
    uint8_t cr = (argc>3)? (uint8_t)std::atoi(argv[3]):2;
    bool has_crc = (argc>4)? (std::atoi(argv[4])!=0):true;
    bool implicit_hdr = (argc>5)? (std::atoi(argv[5])!=0):false;
    uint8_t ldro = (argc>6)? (uint8_t)std::atoi(argv[6]):0;
    uint16_t sync_word1 = (argc>7)? (uint16_t)std::atoi(argv[7]):0x12;
    uint16_t sync_word2 = (argc>8)? (uint16_t)std::atoi(argv[8]):0x34;
    
    std::cout << "Running with SF=" << (int)sf << " CR=" << (int)cr << " CRC=" << has_crc 
             << " IMPL=" << implicit_hdr << " LDRO=" << (int)ldro 
             << " SYNC=[" << sync_word1 << "," << sync_word2 << "]\n";
             
    // Detect file extension and use appropriate reader
    std::string filename(path);
    bool is_unknown_format = (filename.find(".unknown") != std::string::npos);
    
    std::vector<std::complex<float>> full_data;
    std::vector<float> buf;
    
    if (is_unknown_format) {
        // Read as complex float32 little-endian
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) { std::cerr << "Could not open file: " << path << "\n"; return 1; }
        
        std::complex<float> sample;
        while (file.read(reinterpret_cast<char*>(&sample), sizeof(sample))) {
            full_data.push_back(sample);
        }
        file.close();
        std::cout << "Read " << full_data.size() << " complex samples from .unknown format\n";
        
        // Convert to old float format for compatibility
        for (const auto& c : full_data) {
            buf.push_back(c.real());
            buf.push_back(c.imag());
        }
        
        std::cout << "Converted to " << (buf.size()/2) << " complex samples for processing\n";
    } else {
        // Original cf32 format handling
        std::ifstream f(path, std::ios::binary); 
        if(!f){ 
            std::cerr << "Cannot open file: " << path << "\n"; 
            return 1; 
        }
        
        buf = std::vector<float>((std::istreambuf_iterator<char>(f)), {});
        f.close();
        std::cout << "Read " << (buf.size()/2) << " complex samples from cf32 format\n";
    }
    if(buf.empty()){ 
        std::cerr<<"Empty file or read error\n"; 
        return 1; 
    }
    
    size_t byte_count = buf.size();
    if(byte_count % sizeof(float)!=0){ 
        std::cerr<<"File size not multiple of 4 bytes\n"; 
    }
    
    size_t float_count = byte_count / sizeof(float);
    float* fptr = reinterpret_cast<float*>(buf.data());
    if(float_count % 2){ 
        std::cerr<<"Float count not even (I/Q mismatch)\n"; 
        return 1; 
    }
    
    size_t complex_count = float_count / 2;
    std::vector<std::complex<float>> raw(complex_count);
    for(size_t i=0; i<complex_count; i++) {
        raw[i] = { fptr[2*i], fptr[2*i+1] };
    }
    
    std::cout << "Loaded " << complex_count << " IQ samples" << std::endl;
    
    // Set up receiver parameters
    RxParams params;
    params.sf = sf;
    params.cr = cr;
    params.has_crc = has_crc;
    params.implicit_hdr = implicit_hdr;
    params.ldro = ldro;
    params.oversample = 4; // Default oversampling
    params.sync_words = {sync_word1, sync_word2}; // Use command line sync words
    
    std::cout << "Using sync words: " << params.sync_words[0] << ", " << params.sync_words[1] << std::endl;
    
    ReceiverLite receiver(params);
    
    // Process samples in chunks
    const size_t chunk_size = (1u << sf) * params.oversample; // Process 1 symbol at a time
    size_t processed = 0;
    bool frame_found = false;
    
    std::cout << "Processing with chunk size: " << chunk_size << " samples" << std::endl;
    
    while (processed < raw.size()) {
        size_t remaining = raw.size() - processed;
        size_t to_process = std::min(chunk_size, remaining);
        
        std::vector<std::complex<float>> chunk(raw.begin() + processed, 
                                              raw.begin() + processed + to_process);
        
        auto result = receiver.process_samples(chunk);
        
        if (result.frame_detected && !frame_found) {
            frame_found = true;
            std::cout << "Frame detected! CFO: " << result.cfo_int 
                     << " + " << result.cfo_frac 
                     << ", SNR: " << result.snr_est << " dB" << std::endl;
        } else if (processed % (chunk_size * 8) == 0) {
            // Show progress every 8 symbols
            std::cout << "Processed " << processed << "/" << raw.size() 
                     << " samples (" << (100 * processed / raw.size()) << "%) - no frame yet" << std::endl;
        }
        
        if (result.ok) {
            std::cout << "Frame decoded successfully!" << std::endl;
            std::cout << "CRC OK: " << (result.crc_ok ? "Yes" : "No") << std::endl;
            std::cout << "Payload size: " << result.payload.size() << " bytes" << std::endl;
            
            if (!result.payload.empty()) {
                std::cout << "Payload: ";
                for (uint8_t byte : result.payload) {
                    if (byte >= 32 && byte < 127) {
                        std::cout << static_cast<char>(byte);
                    } else {
                        std::cout << "\\x" << std::hex << static_cast<int>(byte) << std::dec;
                    }
                }
                std::cout << std::endl;
            }
            
            break; // Found and decoded a frame
        }
        
        processed += to_process;
        
        // Show progress less frequently now that we added debug above
        if (processed % (chunk_size * 16) == 0) {
            std::cout << "Processed " << processed << "/" << raw.size() 
                     << " samples (" << (100 * processed / raw.size()) << "%)" << std::endl;
        }
    }
    
    if (!frame_found) {
        std::cout << "No frame detected in " << complex_count << " samples" << std::endl;
        return 1;
    }
    
    return 0;
}
