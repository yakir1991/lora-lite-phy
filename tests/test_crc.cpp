#include <iostream>
#include "crc16.hpp"

int main(){
    const unsigned char data[] = { 'H','e','l','l','o' };
    auto crc = lora_lite::crc16_ibm({data,5});
    if (crc == 0) { std::cerr << "crc shouldn't be zero for Hello\n"; return 1; }
    return 0;
}
