#include "hamming.hpp"
#include "json_util.hpp"
#include <filesystem>
#include <iostream>
#include <regex>
#include <string>
#include <vector>
#include <cstdint>

using namespace lora_lite;
namespace fs = std::filesystem;

static uint8_t rev4(uint8_t v){ return (uint8_t)(((v&0x1)<<3)|((v&0x2)<<1)|((v&0x4)>>1)|((v&0x8)>>3)); }

struct Params { int sf; int cr; bool crc; bool impl; int ldro; };

static bool parse_name(const std::string& name, Params& p){
    std::regex re("tx_sf(\\d+)_bw(\\d+)_cr(\\d)_crc(\\d)_impl(\\d)_ldro(\\d+)"); // fallback (unused currently)
    (void)re;
    // We rely on canonical pattern already in filenames: tx_sf7_bw125000_cr2_crc1_impl0_ldro0_pay11_stage.json
    std::regex re2(R"(tx_sf(\d+)_bw(\d+)_cr(\d+)_crc(\d+)_impl(\d+)_ldro(\d+)_pay(\d+)_stage\.json)");
    std::smatch m; if(!std::regex_search(name,m,re2)) return false; p.sf=std::stoi(m[1]); p.cr=std::stoi(m[3]); p.crc= m[4]=="1"; p.impl= m[5]=="1"; p.ldro= std::stoi(m[6]); return true; }

int main(){
    fs::path root = fs::path("stage_dump");
    if(!fs::exists(root)) root = fs::path("../stage_dump");
    if(!fs::exists(root)){ std::cerr<<"stage_dump dir missing"; return 1; }
    std::vector<fs::path> files;
    for(auto& e: fs::directory_iterator(root)) if(e.path().extension()==".json" && e.path().filename().string().find("_stage.json")!=std::string::npos) files.push_back(e.path());
    if(files.empty()){ std::cerr<<"no stage json files"; return 1; }

    HammingTables tables = build_hamming_tables();
    size_t total=0, passed=0;
    for(auto& f: files){
        Params p{}; if(!parse_name(f.filename().string(), p)){ continue; }
        std::string txt; if(!json_slurp(f.string(), txt)){ std::cerr<<"skip (read fail) "<<f<<"\n"; continue; }
        auto deint = json_extract_u8(txt, "deinterleave_b");
        auto ham_ref = json_extract_u8(txt, "hamming_b");
        if(deint.empty()||ham_ref.empty()){ std::cerr<<"skip (missing arrays) "<<f<<"\n"; continue; }
        int sf_app_header = (p.impl||p.ldro) ? p.sf-2 : p.sf-2; // header always sf-2
        int sf_app_payload = (p.ldro? p.sf-2 : p.sf);
        int header_cw_len = 8;
        int payload_cw_len = 4 + p.cr;
        if((int)deint.size() < sf_app_header || (int)ham_ref.size() < sf_app_header){ std::cerr<<"skip (short header)"<<f<<"\n"; continue; }
        // Decode function
        auto decode_block=[&](const std::vector<uint8_t>& block, bool header){
            std::vector<uint8_t> out; out.reserve(block.size());
            CodeRate use = header ? CodeRate::CR48 : (p.cr==1?CodeRate::CR45: p.cr==2?CodeRate::CR46: p.cr==3?CodeRate::CR47: CodeRate::CR48);
            for(uint8_t b: block){ auto dn = hamming_decode4(b, use, tables); out.push_back(dn?rev4(*dn):0xFF); }
            return out;
        };
        auto hdr_block_deint = std::vector<uint8_t>(deint.begin(), deint.begin()+sf_app_header);
        auto hdr_block_ref = std::vector<uint8_t>(ham_ref.begin(), ham_ref.begin()+sf_app_header);
        auto hdr_dec = decode_block(hdr_block_deint, true);
        bool hdr_ok = hdr_dec == hdr_block_ref;
        // Payload blocks
        size_t off = sf_app_header;
        size_t payload_nibbles_ref = ham_ref.size() - sf_app_header;
        size_t payload_blocks = (payload_nibbles_ref + sf_app_payload -1)/sf_app_payload;
        bool payload_ok = true;
        for(size_t b=0; b<payload_blocks; ++b){
            if(off + sf_app_payload > deint.size() || off + sf_app_payload > ham_ref.size()) break;
            std::vector<uint8_t> de_block(deint.begin()+off, deint.begin()+off+sf_app_payload);
            std::vector<uint8_t> ref_block(ham_ref.begin()+off, ham_ref.begin()+off+sf_app_payload);
            auto dec_block = decode_block(de_block,false);
            if(dec_block != ref_block){ payload_ok=false; break; }
            off += sf_app_payload;
        }
        total++;
        if(hdr_ok && payload_ok){ passed++; }
        std::cout<<f.filename().string()<<" : "<<(hdr_ok&&payload_ok?"PASS":"FAIL")<<"\n";
    }
    std::cout<<"Summary Hamming param grid: "<<passed<<"/"<<total<<" passed\n";
    return (passed==total)?0:1;
}
