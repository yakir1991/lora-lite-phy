#include "receiver_lite.hpp"
#include "json_util.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <complex>

// Smoke test: use existing stage dump to feed ReceiverLite from fft_in_c samples and compare payload.
int main(){
    std::string dump = "stage_dump/tx_sf7_bw125000_cr2_crc1_impl0_ldro0_pay11_stage.json";
    std::string txt; if(!json_slurp(dump, txt)){ if(!json_slurp("../"+dump, txt)){ std::cerr<<"missing dump"; return 0; }}
    auto frame_info_pos = txt.find("\"frame_info\"");
    int sf=7; int cr=2; bool has_crc=true; bool impl=false; // inferred from filename
    // Extract fft_in_c arrays
    auto pos = txt.find("\"fft_in_c\""); if(pos==std::string::npos){ std::cerr<<"no fft_in_c"; return 1; }
    auto pos_i = txt.find("\"i\"", pos); auto pos_q = txt.find("\"q\"", pos);
    if(pos_i==std::string::npos||pos_q==std::string::npos){ std::cerr<<"no i/q"; return 1; }
    auto extract = [&](size_t start){ size_t lb=txt.find('[',start); if(lb==std::string::npos) return std::vector<float>{}; size_t p=lb+1; int d=1; while(p<txt.size()&&d>0){ if(txt[p]=='[') d++; else if(txt[p]==']') d--; ++p;} if(d) return std::vector<float>{}; std::string arr=txt.substr(lb+1,p-lb-2); std::regex re("(-?\\d+(?:\\.\\d+)?(?:e-?\\d+)?)"); std::vector<float> vals; for(auto it=std::sregex_iterator(arr.begin(),arr.end(),re); it!=std::sregex_iterator(); ++it) vals.push_back(std::stof((*it)[1])); return vals; };
    auto iv = extract(pos_i); auto qv = extract(pos_q); if(iv.size()!=qv.size()){ std::cerr<<"iq mismatch"; return 1; }
    std::vector<std::complex<float>> samples(iv.size()); for(size_t k=0;k<iv.size();++k) samples[k]={iv[k],qv[k]};
    size_t N = 1u<<sf; if(samples.size()%N!=0){ std::cerr<<"len mismatch"; return 1; }
    size_t sym_cnt = samples.size()/N;
    lora_lite::RxParams prm{(uint8_t)sf,(uint8_t)cr,has_crc,impl,0};
    lora_lite::ReceiverLite rx(prm);
    auto res = rx.decode(samples.data(), sym_cnt);
    std::cout<<"ReceiverLite ok="<<res.ok<<" crc="<<res.crc_ok<<" payload_len="<<res.payload.size()<<"\n";
    return 0; // smoke only
}
