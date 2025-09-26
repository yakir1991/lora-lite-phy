// Utility helpers for packing LSB-first bit vectors into bytes (and inverse if needed).
#pragma once
#include <vector>
#include <cstdint>

namespace lora_lite {

// Pack bits (each entry 0/1) into bytes, LSB-first within each byte.
// Extra bits (if not multiple of 8) fill the high portion of final byte.
inline std::vector<uint8_t> pack_bits_lsb_first(const std::vector<uint8_t>& bits){
    std::vector<uint8_t> out; out.reserve((bits.size()+7)/8);
    uint8_t cur=0; int sh=0; for(uint8_t b: bits){ cur |= (b & 1u) << sh; sh++; if(sh==8){ out.push_back(cur); cur=0; sh=0; } }
    if(sh) out.push_back(cur);
    return out;
}

// (Optional future) unpack; not currently used.
inline std::vector<uint8_t> unpack_bits_lsb_first(const std::vector<uint8_t>& bytes, size_t bit_count){
    std::vector<uint8_t> bits; bits.reserve(bit_count);
    size_t produced=0; for(uint8_t v: bytes){ for(int i=0;i<8 && produced<bit_count;i++){ bits.push_back( (v>>i)&1u ); produced++; } if(produced>=bit_count) break; }
    return bits;
}

}
