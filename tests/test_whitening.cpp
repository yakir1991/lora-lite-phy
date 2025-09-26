#include <cassert>
#include <vector>
#include <iostream>
#include "whitening.hpp"

int main(){
    std::vector<uint8_t> v{0x00,0xFF,0x55,0xAA};
    auto w = lora_lite::whiten(v,0x01);
    auto d = lora_lite::dewhiten(w,0x01);
    if (d!=v){ std::cerr << "whitening roundtrip failed\n"; return 1; }
    return 0;
}
