#include "json_util.hpp"
#include "hamming.hpp"
#include "whitening.hpp"
#include "crc16.hpp"
#include <filesystem>
#include <iostream>
#include <vector>
#include <string>
#include <regex>
#include <optional>
#include <span>

using namespace lora_lite;
namespace fs = std::filesystem;

static uint8_t rev4(uint8_t v){ return (uint8_t)(((v&0x1)<<3)|((v&0x2)<<1)|((v&0x4)>>1)|((v&0x8)>>3)); }

// Map int CR (1..4) to CodeRate enum
static CodeRate map_cr(int cr){ switch(cr){ case 1: return CodeRate::CR45; case 2: return CodeRate::CR46; case 3: return CodeRate::CR47; default: return CodeRate::CR48; } }

static uint16_t crc16_lora_style(std::span<const uint8_t> payload){
    if(payload.size() < 2) return 0;
    uint16_t crc = 0;
    const size_t body_len = payload.size() - 2;
    for(size_t i=0; i<body_len; ++i){
        uint8_t new_byte = payload[i];
        for(int bit=0; bit<8; ++bit){
            bool feedback = (((crc & 0x8000u) >> 8) ^ (new_byte & 0x80u)) != 0;
            crc = static_cast<uint16_t>((crc << 1) & 0xFFFFu);
            if(feedback) crc = static_cast<uint16_t>(crc ^ 0x1021u);
            new_byte = static_cast<uint8_t>((new_byte << 1) & 0xFFu);
        }
    }
    crc ^= payload[payload.size()-1];
    crc ^= static_cast<uint16_t>(payload[payload.size()-2]) << 8;
    return static_cast<uint16_t>(crc & 0xFFFFu);
}

struct Params { int sf; int cr; bool crc; bool impl; int ldro; int pay_len; };
static bool parse_name(const std::string& name, Params& p){
    std::regex re(R"(tx_sf(\d+)_bw(\d+)_cr(\d+)_crc(\d+)_impl(\d+)_ldro(\d+)_pay(\d+)_stage\.json)");
    std::smatch m; if(!std::regex_search(name,m,re)) return false; p.sf=std::stoi(m[1]); p.cr=std::stoi(m[3]); p.crc=(m[4]=="1"); p.impl=(m[5]=="1"); p.ldro=std::stoi(m[6]); p.pay_len=std::stoi(m[7]); return true; }

int main(){
    fs::path root = fs::path("stage_dump"); if(!fs::exists(root)) root = fs::path("../stage_dump"); if(!fs::exists(root)){ std::cerr<<"stage_dump dir missing"; return 1; }
    // Take one (or iterate all) JSON stage files and attempt full chain reconstruction.
    HammingTables tables = build_hamming_tables();

    size_t total=0, passed=0;
    // Deterministic full-chain parity using oracle field 'post_hamming_bits_b'
    // (LSB-first data bits per codeword after Hamming decode).
    for(auto& e: fs::directory_iterator(root)){
        if(e.path().extension() != ".json") continue;
        if(e.path().filename().string().find("_stage.json")==std::string::npos) continue;
        Params prm{}; if(!parse_name(e.path().filename().string(), prm)) continue;
        std::string txt; if(!json_slurp(e.path().string(), txt)) continue;
        auto post_bits = json_extract_u8(txt, "post_hamming_bits_b");
        auto ham_ref = json_extract_u8(txt, "hamming_b");
        if(ham_ref.empty()){ std::cerr<<"skip (no hamming_b) "<<e.path()<<"\n"; continue; }
        if(post_bits.empty()){
            // Synthesize post_hamming_bits_b from nibble list (LSB-first per nibble)
            post_bits.reserve(ham_ref.size()*4);
            for(uint8_t nib : ham_ref){ for(int b=0;b<4;b++) post_bits.push_back( (nib>>b)&1u ); }
        }
        // Sanity: size(post_bits) should be 4 * size(ham_ref)
        if(post_bits.size() < ham_ref.size()*4){ std::cerr<<"skip (short bitstream)"; continue; }
        // Extract payload_hex expected bytes
        auto pay_hex_pos = txt.find("\"payload_hex\"");
        std::vector<uint8_t> expected_payload;
        if(pay_hex_pos!=std::string::npos){
            auto colon = txt.find(':', pay_hex_pos); if(colon!=std::string::npos){
                auto q1 = txt.find('"', colon+1); auto q2 = txt.find('"', q1+1);
                if(q1!=std::string::npos && q2!=std::string::npos){
                    std::string hexs = txt.substr(q1+1, q2-q1-1); std::istringstream iss(hexs); std::string byte_hex; while(iss >> byte_hex){ if(byte_hex.size()==2) expected_payload.push_back((uint8_t)std::stoul(byte_hex,nullptr,16)); }
                }
            }
        }
        size_t needed = (size_t)prm.pay_len + (prm.crc?2:0);
        if(expected_payload.size() && expected_payload.size() != (size_t)prm.pay_len){ /* mismatched meta; continue but note */ }
        // Skip header bits (explicit header only) -> header codewords = sf-2 => header bits = 4*(sf-2)
        size_t header_bits = (prm.impl?0:(size_t)(prm.sf - 2)*4);
        if(post_bits.size() <= header_bits){ std::cerr<<"skip (only header bits)"; continue; }
        std::vector<uint8_t> payload_bits(post_bits.begin()+header_bits, post_bits.end());
        // Pack bits (LSB-first within each byte as produced) into bytes
        std::vector<uint8_t> packed; packed.reserve((payload_bits.size()+7)/8);
        uint8_t cur=0; int sh=0; for(uint8_t b: payload_bits){ cur |= (b&1u) << sh; sh++; if(sh==8){ packed.push_back(cur); cur=0; sh=0; if(packed.size()==needed) break; } }
        if(sh && packed.size()<needed) packed.push_back(cur);
        if(packed.size()<needed){ std::cerr<<"skip (insufficient bytes)"; continue; }
        packed.resize(needed);
        auto packed_wh = packed;
        auto dewhite = dewhiten(packed);
        bool payload_ok=false, crc_ok=false;
        if(expected_payload.size() && dewhite.size()>=expected_payload.size()){
            payload_ok = std::equal(expected_payload.begin(), expected_payload.end(), dewhite.begin());
            if(payload_ok){
                if(prm.crc){
                    if(packed_wh.size() >= expected_payload.size()+2){
                        uint16_t calc_crc = crc16_lora_style({dewhite.data(), expected_payload.size()});
                        uint16_t rx_crc = (uint16_t)packed_wh[expected_payload.size()] | ((uint16_t)packed_wh[expected_payload.size()+1] << 8);
                        crc_ok = (rx_crc == calc_crc);
                    }
                } else crc_ok=true;
            }
        }
        bool pass = payload_ok && crc_ok;
        std::cout<<e.path().filename().string()<<" : "<<(pass?"PASS":"FAIL")
                 <<" pay="<<(payload_ok?"Y":"N")
                 <<" crc="<<(prm.crc?(crc_ok?"Y":"N"):"-")
                 <<"\n";
        total++; if(pass) passed++;
    }
    if(total==0){ std::cerr<<"No stage files processed"; return 1; }
    std::cout<<"Summary full chain: "<<passed<<"/"<<total<<" passed\n";
    return (passed==total)?0:1;
}
