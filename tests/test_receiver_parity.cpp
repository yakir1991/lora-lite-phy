#include "receiver_lite.hpp"
#include "json_util.hpp"
#include <filesystem>
#include <regex>
#include <iostream>
#include <vector>
#include <complex>

using namespace lora_lite;
namespace fs = std::filesystem;

struct Params { int sf; int cr; bool crc; bool impl; int ldro; int pay_len; };
static bool parse_name(const std::string& name, Params& p){
    std::regex re(R"(tx_sf(\d+)_bw(\d+)_cr(\d+)_crc(\d+)_impl(\d+)_ldro(\d+)_pay(\d+)_stage\.json)");
    std::smatch m; if(!std::regex_search(name,m,re)) return false; p.sf=std::stoi(m[1]); p.cr=std::stoi(m[3]); p.crc=(m[4]=="1"); p.impl=(m[5]=="1"); p.ldro=std::stoi(m[6]); p.pay_len=std::stoi(m[7]); return true; }

int main(){
    fs::path root = fs::path("stage_dump"); if(!fs::exists(root)) root = fs::path("../stage_dump"); if(!fs::exists(root)){ std::cerr<<"stage_dump dir missing"; return 0; }
    size_t total=0, passed=0, skipped=0;
    for(auto& e: fs::directory_iterator(root)){
        if(e.path().extension() != ".json") continue;
        if(e.path().filename().string().find("_stage.json")==std::string::npos) continue;
        Params prm{}; if(!parse_name(e.path().filename().string(), prm)) continue;
        std::string txt; if(!json_slurp(e.path().string(), txt)) continue;
        // Extract payload_hex reference
        auto pay_hex_pos = txt.find("\"payload_hex\"");
        std::vector<uint8_t> expected_payload;
        if(pay_hex_pos!=std::string::npos){
            auto colon = txt.find(':', pay_hex_pos); if(colon!=std::string::npos){ auto q1=txt.find('"',colon+1); auto q2=txt.find('"', q1+1); if(q1!=std::string::npos&&q2!=std::string::npos){ std::string hexs=txt.substr(q1+1,q2-q1-1); std::istringstream iss(hexs); std::string bh; while(iss>>bh){ if(bh.size()==2) expected_payload.push_back((uint8_t)std::stoul(bh,nullptr,16)); } } }
        }
        if(expected_payload.empty()) { skipped++; continue; }
        // Extract iq samples
        auto pos = txt.find("\"fft_in_c\""); if(pos==std::string::npos){ skipped++; continue; }
        auto pos_i = txt.find("\"i\"", pos); auto pos_q = txt.find("\"q\"", pos); if(pos_i==std::string::npos||pos_q==std::string::npos){ skipped++; continue; }
        auto extract_f = [&](size_t start){ size_t lb=txt.find('[',start); if(lb==std::string::npos) return std::vector<float>{}; size_t p=lb+1; int d=1; while(p<txt.size()&&d>0){ if(txt[p]=='[') d++; else if(txt[p]==']') d--; ++p;} if(d) return std::vector<float>{}; std::string arr=txt.substr(lb+1,p-lb-2); std::regex re("(-?\\d+(?:\\.\\d+)?(?:e-?\\d+)?)"); std::vector<float> vals; for(auto it=std::sregex_iterator(arr.begin(),arr.end(),re); it!=std::sregex_iterator(); ++it) vals.push_back(std::stof((*it)[1])); return vals; };
        auto iv = extract_f(pos_i); auto qv = extract_f(pos_q); if(iv.size()!=qv.size()||iv.empty()){ skipped++; continue; }
        std::vector<std::complex<float>> samples(iv.size()); for(size_t k=0;k<iv.size();++k) samples[k]={iv[k],qv[k]};
        size_t N = 1u << prm.sf; if(samples.size()%N!=0){ skipped++; continue; }
        size_t sym_cnt = samples.size()/N;
        RxParams rxp{(uint8_t)prm.sf,(uint8_t)prm.cr,prm.crc,prm.impl,(uint8_t)prm.ldro};
        ReceiverLite rx(rxp);
        auto r = rx.decode(samples.data(), sym_cnt);
        bool payload_ok = r.payload.size()==expected_payload.size() && std::equal(expected_payload.begin(), expected_payload.end(), r.payload.begin());
        bool pass = r.ok && r.crc_ok && payload_ok;
        std::cout<<e.path().filename().string()<<" : "<<(pass?"PASS":"FAIL")
                 <<" pay="<<(payload_ok?"Y":"N")
                 <<" crc="<<(prm.crc?(r.crc_ok?"Y":"N"):"-")
                 <<" rx_bytes="<<r.payload.size() <<" exp="<<expected_payload.size() <<"\n";
        total++; if(pass) passed++;
    }
    std::cout<<"Receiver parity summary: "<<passed<<"/"<<total<<" passed (skipped="<<skipped<<")\n";
    return (passed==total)?0:1;
}
