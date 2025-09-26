#include <iostream>
#include "hamming.hpp"

int main(){
    auto tables = lora_lite::build_hamming_tables();
    using CR = lora_lite::CodeRate;
    for (int n=0;n<16;++n){
        for (CR cr: {CR::CR45,CR::CR46,CR::CR47,CR::CR48}){
            auto code = lora_lite::hamming_encode4(n, cr, tables);
            auto dec = lora_lite::hamming_decode4(code, cr, tables);
            if (!dec || *dec != (n & 0xF)){ std::cerr<<"hamming fail nib="<<n<<"\n"; return 1; }
        }
    }
    return 0;
}
