#include <iostream>
#include <fstream>
#include <vector>
#include <complex>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <random>
#include <chrono>
#include <iomanip>
#include <sstream>

// LoRa Lite includes
#include "lora/workspace.hpp"
#include "lora/tx/loopback_tx.hpp"
#include "lora/rx/loopback_rx.hpp"
#include "lora/utils/crc.hpp"
#include "lora/utils/whitening.hpp"
#include "lora/constants.hpp"

using namespace std;

struct LoRaParams {
    uint32_t sf;
    lora::utils::CodeRate cr;
    uint32_t bw;
    uint32_t samp_rate;
    uint8_t sync_word;
    uint8_t preamble_len;
    size_t payload_len;
};

class GoldenVectorGenerator {
private:
    LoRaParams params;
    lora::Workspace ws;
    vector<uint8_t> payload;
    vector<complex<float>> iq_samples;
    
public:
    GoldenVectorGenerator(const LoRaParams& p) : params(p) {
        // Initialize workspace
        ws.init(params.sf);
        
        // Generate random payload
        generate_payload();
    }
    
    void generate_payload() {
        // Use specific "Hello LoRa!" payload instead of random
        std::vector<uint8_t> hello_payload = {
            0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x4c, 0x6f, 0x52, 0x61, 0x21  // "Hello LoRa!"
        };
        
        if (params.payload_len == 11) {
            // Use our specific payload for 11-byte requests
            payload = hello_payload;
            cout << "Using specific 'Hello LoRa!' payload" << endl;
        } else {
            // Fall back to random for other sizes
            payload.resize(params.payload_len);
            random_device rd;
            mt19937 gen(rd());
            uniform_int_distribution<> dis(0, 255);
            
            for (size_t i = 0; i < params.payload_len; ++i) {
                payload[i] = dis(gen);
            }
            cout << "Using random payload for length " << params.payload_len << endl;
        }
    }
    
    void create_golden_vector() {
        cout << "Creating golden vector:" << endl;
        cout << "  SF: " << params.sf << endl;
        cout << "  CR: " << static_cast<int>(params.cr) << endl;
        cout << "  BW: " << params.bw << " Hz" << endl;
        cout << "  Sample rate: " << params.samp_rate << " Hz" << endl;
        cout << "  Sync word: 0x" << hex << static_cast<int>(params.sync_word) << dec << endl;
        cout << "  Preamble length: " << static_cast<int>(params.preamble_len) << endl;
        cout << "  Payload length: " << params.payload_len << " bytes" << endl;
        
        // Create the complete frame
        create_frame();
        
        // Validate the vector
        if (validate_vector()) {
            cout << "✓ Golden vector created and validated successfully!" << endl;
        } else {
            cout << "✗ Validation failed!" << endl;
        }
    }
    
private:
    void create_frame() {
        uint32_t N = 1u << params.sf;  // Samples per symbol
        
        // Calculate header symbols (5 bytes header = 10 nibbles, CR 4/5 -> 12.5 symbols -> 13 symbols)
        size_t header_symbols = 8; // Standard LoRa header symbols for SF7 CR 4/5
        
        // Calculate payload symbols (including CRC)
        size_t payload_bits = (params.payload_len + 2) * 8; // +2 for CRC
        size_t payload_symbols = (payload_bits * 5) / (4 * params.sf); // CR 4/5, SF bits per symbol
        if ((payload_bits * 5) % (4 * params.sf) != 0) payload_symbols++; // Round up
        
        size_t total_symbols = params.preamble_len + 2 + 2 + 1 + header_symbols + payload_symbols; // preamble + sync + downchirps + quarter + header + payload
        size_t total_samples = total_symbols * N;
        
        iq_samples.resize(total_samples);
        
        // Create upchirp for preamble (symbol 0)
        vector<complex<float>> upchirp(N);
        for (uint32_t n = 0; n < N; ++n) {
            float phase = 2.0f * M_PI * (n * n / (2.0f * N) - 0.5f * n);
            upchirp[n] = exp(complex<float>(0.0f, phase));
        }
        
        // Create downchirp (conjugate of upchirp)
        vector<complex<float>> downchirp(N);
        for (uint32_t n = 0; n < N; ++n) {
            downchirp[n] = conj(upchirp[n]);
        }
        
        // Fill preamble with upchirps
        for (uint8_t i = 0; i < params.preamble_len; ++i) {
            memcpy(&iq_samples[i * N], upchirp.data(), N * sizeof(complex<float>));
        }
        
        // Add sync word symbols
        size_t sync_start = params.preamble_len * N;
        create_sync_symbols(&iq_samples[sync_start]);
        
        // Add downchirps after sync
        size_t downchirp_start = (params.preamble_len + 2) * N;
        for (int i = 0; i < 2; ++i) {
            memcpy(&iq_samples[downchirp_start + i * N], downchirp.data(), N * sizeof(complex<float>));
        }
        
        // Add quarter downchirp
        size_t quarter_start = downchirp_start + 2 * N;
        memcpy(&iq_samples[quarter_start], downchirp.data(), (N / 4) * sizeof(complex<float>));
    }
    
    void create_sync_symbols(complex<float>* dest) {
        uint32_t N = 1u << params.sf;
        
        // Convert sync word to two symbols (like GNU Radio does)
        uint8_t sync1 = ((params.sync_word & 0xF0) >> 4) << 3;
        uint8_t sync2 = (params.sync_word & 0x0F) << 3;
        
        // Create sync symbol 1
        create_modulated_symbol(dest, sync1);
        
        // Create sync symbol 2  
        create_modulated_symbol(dest + N, sync2);
    }
    
    void create_modulated_symbol(complex<float>* dest, uint8_t symbol) {
        uint32_t N = 1u << params.sf;
        
        // Create upchirp shifted by symbol value
        for (uint32_t n = 0; n < N; ++n) {
            float phase = 2.0f * M_PI * (n * n / (2.0f * N) + (static_cast<float>(symbol) / N - 0.5f) * n);
            dest[n] = exp(complex<float>(0.0f, phase));
        }
    }
    
    bool validate_vector() {
        // Try to decode the vector with our decoder
        auto result = lora::rx::loopback_rx(ws, iq_samples, params.sf, params.cr, params.payload_len, false, params.sync_word);
        
        if (!result.second) {
            cout << "✗ Decoder failed to decode the vector" << endl;
            return false;
        }
        
        // Check if payload matches
        if (result.first.size() != params.payload_len) {
            cout << "✗ Payload length mismatch: expected " << params.payload_len 
                 << ", got " << result.first.size() << endl;
            return false;
        }
        
        for (size_t i = 0; i < params.payload_len; ++i) {
            if (result.first[i] != payload[i]) {
                cout << "✗ Payload content mismatch at byte " << i 
                     << ": expected 0x" << hex << static_cast<int>(payload[i])
                     << ", got 0x" << static_cast<int>(result.first[i]) << dec << endl;
                return false;
            }
        }
        
        cout << "✓ Vector validation successful!" << endl;
        return true;
    }
    
public:
    void save_to_files(const string& iq_filename, const string& payload_filename) {
        // Save IQ samples
        ofstream iq_file(iq_filename, ios::binary);
        if (!iq_file) {
            cerr << "Error: Cannot open IQ file " << iq_filename << endl;
            return;
        }
        iq_file.write(reinterpret_cast<const char*>(iq_samples.data()), 
                     iq_samples.size() * sizeof(complex<float>));
        iq_file.close();
        cout << "✓ IQ samples saved to " << iq_filename << endl;
        
        // Save payload
        ofstream payload_file(payload_filename, ios::binary);
        if (!payload_file) {
            cerr << "Error: Cannot open payload file " << payload_filename << endl;
            return;
        }
        payload_file.write(reinterpret_cast<const char*>(payload.data()), payload.size());
        payload_file.close();
        cout << "✓ Payload saved to " << payload_filename << endl;
    }
    
    void print_payload_hex() {
        cout << "Payload (hex): ";
        for (uint8_t byte : payload) {
            cout << hex << setw(2) << setfill('0') << static_cast<int>(byte) << " ";
        }
        cout << dec << endl;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 4) {
        cout << "Usage: " << argv[0] << " <sf> <cr> <payload_len> [sync_word] [preamble_len]" << endl;
        cout << "  sf: spreading factor (7-12)" << endl;
        cout << "  cr: coding rate (45,46,47,48)" << endl;
        cout << "  payload_len: payload length in bytes" << endl;
        cout << "  sync_word: sync word (0x12 or 0x34, default 0x12)" << endl;
        cout << "  preamble_len: preamble length (default 8)" << endl;
        return 1;
    }
    
    LoRaParams params;
    params.sf = stoi(argv[1]);
    params.cr = static_cast<lora::utils::CodeRate>(stoi(argv[2]) - 44);
    params.payload_len = stoul(argv[3]);
    params.sync_word = (argc > 4) ? stoul(argv[4], nullptr, 16) : 0x12;
    params.preamble_len = (argc > 5) ? stoul(argv[5]) : 8;
    params.bw = 125000;
    params.samp_rate = 125000;
    
    // Validate parameters
    if (params.sf < 7 || params.sf > 12) {
        cerr << "Error: SF must be between 7 and 12" << endl;
        return 1;
    }
    
    if (params.cr < lora::utils::CodeRate::CR45 || params.cr > lora::utils::CodeRate::CR48) {
        cerr << "Error: CR must be 45, 46, 47, or 48" << endl;
        return 1;
    }
    
    if (params.sync_word != 0x12 && params.sync_word != 0x34) {
        cerr << "Error: Sync word must be 0x12 or 0x34" << endl;
        return 1;
    }
    
    // Create output directory
    if (system("mkdir -p vectors") != 0) {
        cerr << "Warning: Failed to create vectors directory" << endl;
    }
    
    // Generate golden vector
    GoldenVectorGenerator generator(params);
    generator.create_golden_vector();
    generator.print_payload_hex();
    
    // Save to files
    ostringstream sync_hex;
    sync_hex << hex << static_cast<int>(params.sync_word);
    string iq_filename = "vectors/sf" + to_string(params.sf) + "_cr" + to_string(static_cast<int>(params.cr) + 40) + 
                        "_iq_sync" + sync_hex.str() + ".bin";
    string payload_filename = "vectors/sf" + to_string(params.sf) + "_cr" + to_string(static_cast<int>(params.cr) + 40) + 
                            "_payload_sync" + sync_hex.str() + ".bin";
    
    generator.save_to_files(iq_filename, payload_filename);
    
    cout << "\n✓ Golden vector creation completed!" << endl;
    cout << "  IQ file: " << iq_filename << endl;
    cout << "  Payload file: " << payload_filename << endl;
    
    return 0;
}
