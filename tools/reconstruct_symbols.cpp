#include <iostream>
#include <vector>
#include <cstdint>
#include <iomanip>

// Gray decode (inverse of encode)
// encode: v ^ (v >> 1)
// decode: v ^ (v >> 1) ^ (v >> 2) ...
uint16_t gray_decode(uint16_t gray)
{
    uint16_t mask = gray >> 1;
    while (mask != 0) {
        gray = gray ^ mask;
        mask = mask >> 1;
    }
    return gray;
}

// Gray encode
uint16_t gray_encode(uint16_t value)
{
    return value ^ (value >> 1);
}

inline int modulo(int a, int m)
{
    int result = a % m;
    if (result < 0) {
        result += m;
    }
    return result;
}

int main() {
    int sf = 10;
    int cw_len = 8; // cr=4 -> 8 symbols
    int sf_app = 10; // sf=10, ldro=false

    // Target codewords from test_vector_deinterleaver.txt
    std::vector<uint8_t> codewords = {116, 156, 166, 99, 232, 116, 0, 255, 23, 166};

    if (codewords.size() != sf_app) {
        std::cerr << "Error: Need " << sf_app << " codewords." << std::endl;
        return 1;
    }

    // Reconstruct Gray-mapped symbols
    // deinter_matrix[row][i] = inter_matrix[i][j]
    // row = (i - j - 1) % sf
    // => j = (i - 1 - row) % sf
    
    // inter_matrix[i][j] is bit j of symbol i.
    // deinter_matrix[row][i] is bit i of codeword row.
    
    std::vector<uint16_t> reconstructed_gray(cw_len, 0);

    for (int i = 0; i < cw_len; ++i) {
        uint16_t symbol_val = 0;
        for (int j = 0; j < sf_app; ++j) {
            // Find which codeword bit corresponds to this symbol bit
            int row = modulo(i - j - 1, sf_app);
            
            // Get bit i from codeword row
            bool bit = (codewords[row] >> (cw_len - 1 - i)) & 0x1;
            
            // Set bit j in symbol i
            // Note: int_to_bits fills MSB at index 0.
            // If j=0 is MSB (bit index sf-1 in value).
            // Let's check int_to_bits convention in deinterleaver.cpp:
            // bits[sf - 1 - k] = (value >> k) & 1.
            // So bits[0] is MSB (value >> sf-1).
            // bits[sf-1] is LSB (value >> 0).
            // So index j in inter_matrix corresponds to bit (sf - 1 - j) of value.
            
            if (bit) {
                symbol_val |= (1 << (sf_app - 1 - j));
            }
        }
        reconstructed_gray[i] = symbol_val;
    }

    std::cout << "Reconstructed Gray Symbols:" << std::endl;
    for (auto s : reconstructed_gray) {
        std::cout << s << " ";
    }
    std::cout << std::endl;

    std::cout << "Reconstructed Raw Symbols (Gray Decoded):" << std::endl;
    for (auto s : reconstructed_gray) {
        std::cout << gray_decode(s) << " ";
    }
    std::cout << std::endl;

    return 0;
}
