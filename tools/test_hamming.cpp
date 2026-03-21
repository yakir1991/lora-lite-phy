#include <iostream>
#include <vector>
#include <cstdint>
#include <algorithm>

uint8_t hamming_decode(uint8_t codeword, int cr_app)
{
    std::vector<bool> bits(cr_app + 4);
    const int num_bits = static_cast<int>(bits.size());
    for (int i = 0; i < num_bits; ++i) {
        bits[i] = (codeword >> (num_bits - 1 - i)) & 0x1u;
    }

    // GNU Radio: data = {bits[3], bits[2], bits[1], bits[0]}
    std::vector<bool> data = {bits[3], bits[2], bits[1], bits[0]};

    bool s0 = false;
    bool s1 = false;
    bool s2 = false;

    switch (cr_app) {
    case 4:
        if ((std::count(bits.begin(), bits.end(), true) % 2) == 0) {
            // Parity check (even parity expected?)
            // If count is even, parity is even.
            // If parity is correct, we break?
            // Wait, code says: if (count % 2 == 0) break;
            // This means if parity is EVEN, we assume it's correct and don't correct?
            // Usually parity bit makes the total count EVEN.
            // If count is ODD, there is an error.
            // So if count is EVEN, we break (no correction).
            break;
        }
        [[fallthrough]];
    case 3:
        s0 = bits[4] ^ bits[0] ^ bits[1] ^ bits[2];
        s1 = bits[5] ^ bits[1] ^ bits[2] ^ bits[3];
        s2 = bits[6] ^ bits[0] ^ bits[1] ^ bits[3];
        {
            const int syndrom = static_cast<int>(s0) | (static_cast<int>(s1) << 1) | (static_cast<int>(s2) << 2);
            switch (syndrom) {
            case 5: data[3].flip(); break;
            case 7: data[2].flip(); break;
            case 3: data[1].flip(); break;
            case 6: data[0].flip(); break;
            default: break;
            }
        }
        break;
    }

    uint8_t result = 0;
    for (bool bit : data) {
        result = static_cast<uint8_t>((result << 1) | (bit ? 1 : 0));
    }
    return result;
}

int main() {
    uint8_t cw = 238;
    uint8_t decoded = hamming_decode(cw, 4);
    std::cout << "Codeword: " << (int)cw << " Decoded: " << (int)decoded << std::endl;
    return 0;
}
