#include <iostream>
#include "gray.hpp"
int main(){
  for (unsigned v=0; v<256; ++v){
    auto g = lora_lite::gray_encode(v);
    auto d = lora_lite::gray_decode(g);
    if (d!=v){ std::cerr << "gray mismatch v="<<v<<"\n"; return 1; }
  }
  return 0;
}
