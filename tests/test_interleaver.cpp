#include <iostream>
#include <vector>
#include <cstring>
#include "interleaver.hpp"

int main(){
    int sf_app=5; // example
    int cw_len=8;
    std::vector<uint8_t> in(sf_app*cw_len);
    for (size_t i=0;i<in.size();++i) in[i]=static_cast<uint8_t>(i & 1);
    std::vector<uint8_t> tmp(in.size()), out(in.size());
    lora_lite::interleave_bits(in.data(), tmp.data(), sf_app, cw_len);
    lora_lite::deinterleave_bits(tmp.data(), out.data(), sf_app, cw_len);
    if (out!=in){ std::cerr<<"interleaver roundtrip failed\n"; return 1; }
    return 0;
}
