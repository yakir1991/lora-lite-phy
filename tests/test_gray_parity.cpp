// DRY version using shared json_util helpers
#include "gray.hpp"
#include "json_util.hpp"
#include <iostream>
#include <string>
#include <vector>

int main(){
    // Use one recent stage dump produced by param grid (explicit header example)
    std::string dump = "stage_dump/tx_sf7_bw125000_cr2_crc1_impl0_ldro0_pay11_stage.json";
    std::string txt; if(!json_slurp(dump, txt)){
        std::string alt = "../" + dump;
        if(!json_slurp(alt, txt)){ std::cerr<<"missing dump: "<<dump; return 1; }
    }
    auto fft_syms = json_extract_u16(txt, "fft_demod_sym");
    auto gray_syms = json_extract_u16(txt, "gray_demap_sym");
    if(fft_syms.empty()||gray_syms.empty()){ std::cerr<<"arrays missing"; return 1; }
    if(fft_syms.size() != gray_syms.size()){ std::cerr<<"length mismatch"; return 1; }
    // Header symbols (first 8) underwent rate reduction (was divided by 4 before Gray). After Gray demap, value is gray_encode((raw_idx-1)/4).
    // For payload (no /4), gray_demap_sym = gray_encode(raw_idx-1).
    size_t n = fft_syms.size();
    size_t mismatches = 0;
    for(size_t i=0;i<n;i++){
        uint16_t enc = (uint16_t) lora_lite::gray_encode(fft_syms[i]);
        if(enc != gray_syms[i]) mismatches++;
    }
    std::cout<<"Gray parity (fft_sym -> gray_encode) mismatches="<<mismatches<<" / "<<n<<"\n";
    if(mismatches==0){ std::cout<<"[PASS] gray parity"<<std::endl; return 0; }
    for(size_t i=0;i<std::min<size_t>(16,n);++i){ if(lora_lite::gray_encode(fft_syms[i])!=gray_syms[i])
        std::cout<<i<<": fft="<<fft_syms[i]<<" enc="<<lora_lite::gray_encode(fft_syms[i])<<" gray="<<gray_syms[i]<<"*"<<"\n"; }
    return 1;
}
