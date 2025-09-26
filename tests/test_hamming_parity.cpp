#include "hamming.hpp"
#include <string>
#include <vector>
#include <iostream>
#include <cstdint>
#include "json_util.hpp"

using namespace lora_lite;


int main(){
    // Use same stage dump as deinterleaver test
    std::string dump = "stage_dump/tx_sf7_bw125000_cr2_crc1_impl0_ldro0_pay11_stage.json";
    std::string txt; if(!json_slurp(dump, txt)){
        dump = std::string("../") + dump; if(!json_slurp(dump, txt)){ std::cerr<<"missing dump"; return 1; }
    }
    auto deint = json_extract_u8(txt, "deinterleave_b");
    auto ham_ref = json_extract_u8(txt, "hamming_b");
    if(deint.empty() || ham_ref.empty()){ std::cerr<<"arrays missing"; return 1; }
    // Parameters from filename: sf=7, cr=2 (=> CodeRate CR46), header first -> sf_app_header=5 then payload sf_app=7
    const int sf=7; const int cr=2; int sf_app_header=sf-2; int sf_app_payload=sf; int header_cw_len=8; int payload_cw_len=4+cr; //6
    // Layout of deinterleave_b: header block first: sf_app_header bytes, then each payload block contributes sf_app_payload bytes.
    if((int)deint.size() < sf_app_header){ std::cerr<<"deinterleave too short"; return 1; }
    HammingTables tables = build_hamming_tables();
    auto decode_block = [&](const std::vector<uint8_t>& block_bytes, int cw_len, int rows, bool is_header) -> std::vector<uint8_t>{
        std::vector<uint8_t> out_nibbles; out_nibbles.reserve(rows);
        for(int r=0;r<rows;r++){
            uint8_t code = block_bytes[r] & 0xFF; // full byte from GR
            CodeRate use = is_header ? CodeRate::CR48 : CodeRate::CR46; // header always treated as 4/8
            auto dn = hamming_decode4(code, use, tables);
            out_nibbles.push_back(dn ? (uint8_t)*dn : (uint8_t)0xFE);
        }
        return out_nibbles;
    };
    // Reference hamming_b is sequence of decoded nibbles (rows) for each block.
    // Extract reference header nibble block (first sf_app_header entries)
    if((int)ham_ref.size() < sf_app_header){ std::cerr<<"hamming ref too short"; return 1; }
    std::vector<uint8_t> ref_header(ham_ref.begin(), ham_ref.begin()+sf_app_header);
    // Decode our header block
    std::vector<uint8_t> deint_header(deint.begin(), deint.begin()+sf_app_header);
    auto dec_header = decode_block(deint_header, header_cw_len, sf_app_header, true);
    auto rev4 = [](uint8_t v){ return (uint8_t)(((v&0x1)<<3)|((v&0x2)<<1)|((v&0x4)>>1)|((v&0x8)>>3)); };
    size_t mism_header=0; for(size_t i=0;i<dec_header.size();++i){ uint8_t r=rev4(dec_header[i]); if(r!=ref_header[i]) mism_header++; }
    std::cout<<"Header hamming mismatches="<<mism_header<<"/"<<dec_header.size()<<"\n";
    // All payload blocks
    size_t payload_total_nibbles = ham_ref.size() - sf_app_header;
    if(payload_total_nibbles % sf_app_payload != 0){ std::cerr<<"payload nibble count not multiple of sf_app_payload"; }
    size_t payload_blocks = payload_total_nibbles / sf_app_payload;
    size_t deint_payload_bytes = deint.size() - sf_app_header;
    if(deint_payload_bytes / sf_app_payload != payload_blocks){
        // If lengths mismatched, clamp to min blocks
        payload_blocks = std::min(deint_payload_bytes / sf_app_payload, payload_blocks);
    }
    size_t total_payload_mism=0, total_payload_checked=0;
    for(size_t b=0;b<payload_blocks;b++){
        size_t off_b = sf_app_header + b*sf_app_payload;
        std::vector<uint8_t> deint_block(deint.begin()+off_b, deint.begin()+off_b+sf_app_payload);
        std::vector<uint8_t> ref_block(ham_ref.begin()+off_b, ham_ref.begin()+off_b+sf_app_payload);
        auto dec_block = decode_block(deint_block, payload_cw_len, sf_app_payload, false);
        size_t mism_block=0; for(size_t i=0;i<dec_block.size();++i){ uint8_t r=rev4(dec_block[i]); if(r!=ref_block[i]) mism_block++; }
        std::cout<<"Payload["<<b<<"] mismatches="<<mism_block<<"/"<<dec_block.size()<<"\n";
        total_payload_mism += mism_block;
        total_payload_checked += dec_block.size();
    }
    std::cout<<"Aggregate payload mismatches="<<total_payload_mism<<"/"<<total_payload_checked<<"\n";
    if(mism_header==0 && total_payload_mism==0){ std::cout<<"[PASS] hamming parity full frame"<<std::endl; return 0; }
    return 1;
}
