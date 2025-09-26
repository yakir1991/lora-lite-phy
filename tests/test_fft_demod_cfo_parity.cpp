// DRY: use shared json_util for array extraction; only small local parser for frame_info needed
#include "fft_demod_lite.hpp"
#include "json_util.hpp"
#include <complex>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

static bool slurp(const std::string& p, std::string& out){ return json_slurp(p,out); }
static std::vector<std::complex<float>> parse_fft_in(const std::string& txt){
    std::vector<std::complex<float>> out; auto pos=txt.find("\"fft_in_c\""); if(pos==std::string::npos) return out; auto pos_i=txt.find("\"i\"",pos); auto pos_q=txt.find("\"q\"",pos); if(pos_i==std::string::npos||pos_q==std::string::npos) return out; auto extract=[&](size_t s){ size_t lb=txt.find('[',s); if(lb==std::string::npos) return std::vector<float>{}; size_t p=lb+1; int d=1; while(p<txt.size()&&d>0){ if(txt[p]=='[') d++; else if(txt[p]==']') d--; ++p;} if(d) return std::vector<float>{}; std::string arr=txt.substr(lb+1,p-lb-2); std::regex re("(-?\\d+(?:\\.\\d+)?(?:e-?\\d+)?)"); std::vector<float> vals; for(auto it=std::sregex_iterator(arr.begin(),arr.end(),re); it!=std::sregex_iterator(); ++it) vals.push_back(std::stof((*it)[1])); return vals; }; auto iv=extract(pos_i); auto qv=extract(pos_q); if(iv.size()!=qv.size()) return out; out.resize(iv.size()); for(size_t k=0;k<iv.size();++k) out[k]={iv[k],qv[k]}; return out; }
struct FrameInfo{int cfo_int=0; float cfo_frac=0.f; int sf=0; bool have=false;};
static FrameInfo parse_frame_info(const std::string& txt){ FrameInfo fi; auto p=txt.find("\"frame_info\""); if(p==std::string::npos) return fi; auto num=[&](const char* key){ auto k=std::string("\"")+key+"\""; auto kp=txt.find(k,p); if(kp==std::string::npos) return 0.0; kp=txt.find(':',kp); if(kp==std::string::npos) return 0.0; std::regex re("(-?\\d+(?:\\.\\d+)?(?:e-?\\d+)?)"); auto tail=txt.substr(kp+1,64); auto it=std::sregex_iterator(tail.begin(),tail.end(),re); if(it==std::sregex_iterator()) return 0.0; return std::stod((*it)[1]); }; fi.cfo_int=(int)num("cfo_int"); fi.cfo_frac=(float)num("cfo_frac"); fi.sf=(int)num("sf"); fi.have=true; return fi; }

int main(){
    std::string dump = "stage_dump/tx_sf7_bw125000_cr2_crc1_impl0_ldro0_pay11_stage.json"; // reusing existing stage dump
    std::string txt; if(!slurp(dump, txt)){
        std::string alt = "../" + dump;
        if(!slurp(alt, txt)){ std::cerr << "missing dump: "<<dump; return 1; }
    }
    auto syms = json_extract_u16(txt, "fft_demod_sym"); if(syms.empty()){ std::cerr<<"no fft_demod_sym"; return 1; }
    auto samples = parse_fft_in(txt); if(samples.empty()){ std::cerr<<"no fft_in_c"; return 1; }
    auto fi = parse_frame_info(txt); if(!fi.have){ std::cerr<<"no frame_info"; return 1; }
    if(fi.sf != 7){ std::cerr<<"unexpected sf"; return 1; }
    size_t N = 1u<<fi.sf; if(samples.size() % N != 0){ std::cerr<<"fft_in len mismatch"; return 1; }
    size_t sym_cnt = samples.size()/N; lora_lite::FftDemodLite dem(fi.sf);
    // Apply CFO from frame_info (mirrors GR path)
    dem.apply_cfo(fi.cfo_int, fi.cfo_frac);
    std::vector<uint16_t> ours; ours.reserve(sym_cnt);
    for(size_t s=0;s<sym_cnt;s++) ours.push_back(dem.demod(&samples[s*N]));
    // Post-process like GR: first header block (implicit header still produces 8 header symbols) uses (idx-1)/4 ; payload uses (idx-1).
    std::vector<uint16_t> ours_proc; ours_proc.reserve(ours.size());
    const size_t header_len=8; for(size_t i=0;i<ours.size(); ++i){ uint16_t adj = (uint16_t)((ours[i] + N - 1) % N); if(i < header_len) adj = (uint16_t)(adj/4); ours_proc.push_back(adj); }
    size_t compare = std::min(ours_proc.size(), syms.size()); size_t lead=0; for(size_t i=0;i<compare;i++){ if(ours_proc[i]==syms[i]) lead++; else break; }
    std::cout << "CFO-parity leading="<<lead<<"/"<<compare<<" (cfo_int="<<fi.cfo_int<<" cfo_frac="<<fi.cfo_frac<<")\n";
    // Expect CRC invalid payload mismatch because implicit header vector here differs from explicit header reference, but parity is against syms themselves.
    if(lead==compare){ std::cout<<"[PASS] full CFO parity"<<std::endl; return 0; }
    for(size_t i=0;i<std::min<size_t>(16, compare); ++i){ std::cout<<i<<": gr="<<syms[i]<<" ours="<<ours_proc[i]<<(ours_proc[i]==syms[i]?"":" *")<<"\n"; }
    return 1;
}
