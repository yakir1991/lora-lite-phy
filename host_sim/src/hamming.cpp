#include "host_sim/hamming.hpp"

#include <algorithm>

namespace host_sim
{

uint8_t hamming_decode(uint8_t codeword, int cr_app)
{
    std::vector<bool> bits(cr_app + 4);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        bits[bits.size() - 1 - i] = (codeword >> i) & 0x1u;
    }

    std::vector<bool> data = {bits[3], bits[2], bits[1], bits[0]};

    bool s0 = false;
    bool s1 = false;
    bool s2 = false;

    switch (cr_app) {
    case 4:
        if ((std::count(bits.begin(), bits.end(), true) % 2) == 0) {
            break;
        }
        [[fallthrough]];
    case 3:
        s0 = bits[0] ^ bits[1] ^ bits[2] ^ bits[4];
        s1 = bits[1] ^ bits[2] ^ bits[3] ^ bits[5];
        s2 = bits[0] ^ bits[1] ^ bits[3] ^ bits[6];
        {
            const int syndrom = static_cast<int>(s0) | (static_cast<int>(s1) << 1) | (static_cast<int>(s2) << 2);
            switch (syndrom) {
            case 5:
                data[3].flip();
                break;
            case 7:
                data[2].flip();
                break;
            case 3:
                data[1].flip();
                break;
            case 6:
                data[0].flip();
                break;
            default:
                break;
            }
        }
        break;
    case 2:
        s0 = bits[0] ^ bits[1] ^ bits[2] ^ bits[4];
        s1 = bits[1] ^ bits[2] ^ bits[3] ^ bits[5];
        if (s0 || s1) {
            // not corrected
        }
        break;
    case 1:
        if ((std::count(bits.begin(), bits.end(), true) % 2) == 0) {
            // parity fail -> ignore
        }
        break;
    default:
        break;
    }

    uint8_t result = 0;
    for (bool bit : data) {
        result = static_cast<uint8_t>((result << 1) | (bit ? 1 : 0));
    }
    return result;
}

std::vector<uint8_t> hamming_decode_block(const std::vector<uint8_t>& codewords, bool header, int cr)
{
    const int cr_app = header ? 4 : cr;
    std::vector<uint8_t> result;
    result.reserve(codewords.size());
    for (uint8_t cw : codewords) {
        result.push_back(hamming_decode(cw, cr_app));
    }
    return result;
}

} // namespace host_sim
