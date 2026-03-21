#include <iostream>
#include <vector>
#include <cstdint>
#include <iomanip>

// Gray decode
uint16_t gray_decode(uint16_t gray)
{
    uint16_t mask = gray >> 1;
    while (mask != 0) {
        gray = gray ^ mask;
        mask = mask >> 1;
    }
    return gray;
}

// Int to bits (MSB first)
std::vector<bool> int_to_bits(uint16_t value, int bit_count)
{
    std::vector<bool> bits(bit_count);
    for (int i = 0; i < bit_count; ++i) {
        bits[bit_count - 1 - i] = (value >> i) & 0x1u;
    }
    return bits;
}

// Bits to uint8
uint8_t bits_to_uint8(const std::vector<bool>& bits)
{
    uint8_t result = 0;
    for (bool bit : bits) {
        result = static_cast<uint8_t>((result << 1) | (bit ? 1 : 0));
    }
    return result;
}

inline int modulo(int a, int m)
{
    int result = a % m;
    if (result < 0) {
        result += m;
    }
    return result;
}

// Deinterleave block of symbols
std::vector<uint8_t> deinterleave(const std::vector<uint16_t>& symbols, int sf, int cr)
{
    int cw_len = cr + 4;
    int sf_app = sf;
    
    std::vector<std::vector<bool>> inter_matrix(cw_len);
    
    for (int i = 0; i < cw_len; ++i) {
        uint16_t raw = symbols[i];
        uint16_t mapped = gray_decode(raw);
        inter_matrix[i] = int_to_bits(mapped, sf_app);
    }

    std::vector<std::vector<bool>> deinter_matrix(sf_app, std::vector<bool>(cw_len));
    for (int i = 0; i < cw_len; ++i) {
        for (int j = 0; j < sf_app; ++j) {
            const int row = modulo(i - j - 1, sf_app);
            deinter_matrix[row][i] = inter_matrix[i][j];
        }
    }

    std::vector<uint8_t> result(sf_app);
    for (int i = 0; i < sf_app; ++i) {
        result[i] = bits_to_uint8(deinter_matrix[i]);
    }
    return result;
}

int main() {
    int sf = 10;
    int cr = 4;
    int cw_len = cr + 4; // 8

    // We want to find symbols that produce codewords 210 and 156 at indices 2 and 3 (0-based)
    // The deinterleaver produces 'sf' codewords.
    // We have 'cw_len' symbols as input.
    // Wait, deinterleaver takes 'cw_len' symbols and produces 'sf' codewords.
    // So we need to find 'cw_len' symbols (8 symbols) that produce the desired codewords.
    
    // We already know the first 2 symbols are correct (908, 587).
    // And we suspect the next 2 are wrong (360, 477).
    // And we have 4 more symbols in the block.
    // The block has 8 symbols.
    // host_sim saw: 908 587 360 477 763 128 656 161
    
    std::vector<uint16_t> current_symbols = {908, 587, 360, 477, 763, 128, 656, 161};
    
    // We want output codewords at index 2 to be 210, and index 3 to be 156.
    // Currently they are 140 and 89.
    
    // We can try to vary symbols at index 2 and 3?
    // No, interleaving spreads bits across ALL symbols.
    // Changing symbol 2 affects ALL codewords (one bit in each codeword).
    
    // This means if we have wrong codewords at index 2 and 3, it could be due to ANY of the 8 symbols being wrong.
    // But likely it's the symbols that contribute to the bits of codeword 2 and 3.
    
    // Let's check which symbols contribute to codeword 2 and 3.
    // deinter_matrix[row][i] = inter_matrix[i][j]
    // row is the codeword index (0..sf-1).
    // i is the symbol index (0..cw_len-1).
    // j is the bit index in the symbol.
    // row = (i - j - 1) % sf.
    
    // We want to fix codeword 2 (row=2).
    // For each symbol i (0..7), which bit j contributes to row 2?
    // 2 = (i - j - 1) % 10.
    // j = (i - 1 - 2) % 10 = (i - 3) % 10.
    
    // For symbol 0 (i=0): j = -3 = 7. Bit 7 of symbol 0.
    // For symbol 1 (i=1): j = -2 = 8. Bit 8 of symbol 1.
    // ...
    // For symbol 2 (i=2): j = -1 = 9. Bit 9 of symbol 2.
    // For symbol 3 (i=3): j = 0. Bit 0 of symbol 3.
    
    // So symbol 2 (360) contributes bit 9 to codeword 2.
    // Symbol 3 (477) contributes bit 0 to codeword 2.
    
    // We want to fix codeword 3 (row=3).
    // 3 = (i - j - 1) % 10.
    // j = (i - 4) % 10.
    
    // Symbol 2 (i=2): j = -2 = 8. Bit 8 of symbol 2.
    // Symbol 3 (i=3): j = -1 = 9. Bit 9 of symbol 3.
    
    // So symbols 2 and 3 contribute heavily to codewords 2 and 3?
    // Actually, every symbol contributes 1 bit to every codeword.
    
    // If we assume only symbols 2 and 3 are wrong (because they looked suspicious in the log? No, I just assumed).
    // Actually, I don't know which symbols are wrong.
    // But `908` and `587` produced correct codewords 0 and 1?
    // Codeword 0 depends on:
    // i=0, j=(0-1-0)=9.
    // i=1, j=(1-1-0)=0.
    // i=2, j=(2-1-0)=1.
    // ...
    
    // If codewords 0 and 1 are correct, it means the bits contributing to them are correct.
    // This constrains the symbols.
    
    // I will try to brute force symbols 2 and 3 (indices 2, 3) while keeping others fixed, 
    // and see if I can get codewords 2 and 3 to match 210 and 156.
    // If not, maybe other symbols are wrong too.
    
    std::cout << "Original symbols: ";
    for(auto s : current_symbols) std::cout << s << " ";
    std::cout << std::endl;
    
    auto result = deinterleave(current_symbols, sf, cr);
    std::cout << "Original codewords: ";
    for(auto c : result) std::cout << (int)c << " ";
    std::cout << std::endl;
    
    // Target: result[2] == 210, result[3] == 156.
    
    // Brute force symbol 2 and 3
    // 1024 * 1024 = 1M iterations. Fast enough.
    
    for (int s2 = 0; s2 < 1024; ++s2) {
        for (int s3 = 0; s3 < 1024; ++s3) {
            current_symbols[2] = s2;
            current_symbols[3] = s3;
            
            auto res = deinterleave(current_symbols, sf, cr);
            
            if (res[2] == 210 && res[3] == 156) {
                std::cout << "FOUND MATCH! s2=" << s2 << " s3=" << s3 << std::endl;
                // Check if other codewords are still reasonable?
                // We don't know what other codewords should be.
                // But we know cw 0 and 1 should ideally stay same?
                // Changing s2 and s3 WILL change cw 0 and 1.
                // If cw 0 and 1 change, then our assumption that s0 and s1 are correct might be violated?
                // Wait, if s0 and s1 are correct, they contribute to cw 0 and 1.
                // s2 and s3 ALSO contribute to cw 0 and 1.
                // So if we change s2 and s3, we MIGHT break cw 0 and 1.
                
                // Let's check if cw 0 and 1 are still 238 and 212.
                if (res[0] == 238 && res[1] == 212) {
                    std::cout << "  AND cw 0, 1 are preserved!" << std::endl;
                } else {
                    std::cout << "  BUT cw 0=" << (int)res[0] << " cw 1=" << (int)res[1] << " (expected 238, 212)" << std::endl;
                }
            }
        }
    }
    
    return 0;
}
