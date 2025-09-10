#pragma once
#include <cstdint>
#include <cstddef>

namespace lora::utils {


struct LfsrWhitening {
    uint32_t poly;   
    uint32_t state;  
    uint32_t order;  

    static LfsrWhitening pn9_default(); 

    void apply(uint8_t* buf, size_t len); 
};

} // namespace lora::utils
