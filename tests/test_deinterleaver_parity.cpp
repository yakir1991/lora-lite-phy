#include "interleaver.hpp"
#include "gray.hpp"
#include <fstream>
#include <regex>
#include <string>
#include <vector>
#include <iostream>
#include <cstdint>
#include "json_util.hpp"


int main(){
    // Use a stage dump with crc+explicit header: has deinterleave_b content
    std::string dump = "stage_dump/tx_sf7_bw125000_cr2_crc1_impl0_ldro0_pay11_stage.json";
    std::string txt; if(!json_slurp(dump, txt)) { dump = std::string("../") + dump; if(!json_slurp(dump, txt)){ std::cerr<<"missing dump"; return 1; } }
    auto fft_syms = json_extract_u16(txt, "fft_demod_sym");
    auto deint_ref = json_extract_u8(txt, "deinterleave_b");
    if(fft_syms.empty()||deint_ref.empty()){ std::cerr<<"needed arrays missing"; return 1; }
    // Parameters: sf=7 => sf_app=5 (sf-2). Header codeword length = 8 symbols.
    // After fft_demod, header symbols are rate-reduced (/4) by GR; we already compare parity there.
    // deinterleave_b in stage dump is a flat byte array: first header deinterleaver output (sf_app*cw_header bytes),
    // then subsequent payload codewords each of length (4+cr) symbols with sf_app bits per symbol serialized similarly.
    auto gray_syms = json_extract_u16(txt, "gray_demap_sym"); if(gray_syms.empty()){ std::cerr<<"gray_demap_sym missing"; return 1; }
    // Take first block (header): 8 symbols each yields (sf_app=5) bits packed across column-major (coding rate part). GR deinterleaver outputs bytes (sf_app or codeword len per batch). Reference dump shows multiple bytes; we only check first 8*5 bits laid out into columns length cw_len=4+cr (here cr=2 so block_size=6? header uses 4?). For simplicity compare first deinterleave_b segment length  (sf_app *  (4+4)=5*8=40 bits -> 5 bytes). We'll just ensure diagonal inverse matches first 40 entries of deinterleave_b if present.
    int sf_app = 5; // for sf>=7
    // Build bit matrix from gray symbols (header first 8 gray_syms) by taking their lower sf_app bits (since rate reduction already applied)
    const int sf = 7; // parse from params (fixed for this vector)
    const size_t header_cw_len = 8; // symbols
    if(gray_syms.size() < header_cw_len){ std::cerr<<"not enough header symbols"; return 1; }
    // Reconstruct header deinterleaver output
    auto build_block = [&](size_t start_sym, size_t cw_len, int sf_app_local){
        std::vector<uint8_t> in_bits(sf_app_local * cw_len); // column-serialized (col*sf_app + row)
        for(size_t col=0; col<cw_len; ++col){
            uint16_t sym = gray_syms[start_sym + col];
            for(int b=0; b<sf_app_local; ++b){
                uint8_t bit = (sym >> b) & 1;
                int row = sf_app_local - 1 - b; // row 0 = MSB
                in_bits[col*sf_app_local + row] = bit;
            }
        }
        std::vector<uint8_t> out_bits(sf_app_local * cw_len);
        lora_lite::deinterleave_bits(in_bits.data(), out_bits.data(), sf_app_local, (int)cw_len);
        return out_bits;
    };
    int sf_app_header = sf - 2;
    int sf_app_payload = sf; // no LDRO in this vector
    auto header_bits = build_block(0, header_cw_len, sf_app_header); // deinterleaved bits
    // Our build() returns out_bits with indexing dest_row*cw_len + col where dest_row runs 0..sf_app-1.
    // That matches deinter_bin[row][col] in GR. So pack each row LSB-first across cw_len bits.
    auto pack_row_msb = [&](const std::vector<uint8_t>& bits, size_t row, size_t cols){
        // bits[row*cols + col] currently correspond to deinter_bin[row][col] with col increasing left->right.
        // GR bool2int treats first element as MSB. So accumulate accordingly.
        uint32_t v=0; for(size_t c=0;c<cols;c++){ v = (v<<1) | (bits[row*cols + c]&1); } return (uint8_t)v; };
    std::vector<uint8_t> header_bytes(sf_app_header);
    for(int r=0;r<sf_app_header;r++) header_bytes[r] = pack_row_msb(header_bits, r, header_cw_len);
    if(deint_ref.size() < header_bytes.size()){ std::cerr<<"reference header bytes missing"; return 1; }
    size_t mism_header=0; for(size_t i=0;i<header_bytes.size();++i) if(header_bytes[i]!=deint_ref[i]) mism_header++;
    std::cout<<"Header bytes:"; for(auto b: header_bytes) std::cout<<" "<<(int)b; std::cout<<"\nRef header :"; for(size_t i=0;i<header_bytes.size();++i) std::cout<<" "<<(int)deint_ref[i]; std::cout<<"\nHeader mismatches="<<mism_header<<"/"<<header_bytes.size()<<"\n";
    // If header mismatches, fail early
    if(mism_header){ return 1; }
    // First payload block (after header)
    size_t cr = 2; size_t payload_cw_len = 4 + cr; size_t payload_start = header_cw_len;
    if(gray_syms.size() < payload_start + payload_cw_len){ std::cerr<<"not enough payload symbols"; return 0; }
    auto payload_bits = build_block(payload_start, payload_cw_len, sf_app_payload);
    std::vector<uint8_t> payload_bytes(sf_app_payload);
    for(int r=0;r<sf_app_payload;r++) payload_bytes[r] = pack_row_msb(payload_bits, r, payload_cw_len);
    size_t ref_off = header_bytes.size();
    if(deint_ref.size() < ref_off + payload_bytes.size()){ std::cerr<<"reference payload bytes missing"; return 0; }
    size_t mism_pay=0; for(size_t i=0;i<payload_bytes.size();++i) if(payload_bytes[i]!=deint_ref[ref_off+i]) mism_pay++;
    std::cout<<"Payload[0] bytes:"; for(auto b: payload_bytes) std::cout<<" "<<(int)b; std::cout<<"\nRef payload[0]:"; for(size_t i=0;i<payload_bytes.size();++i) std::cout<<" "<<(int)deint_ref[ref_off+i]; std::cout<<"\nPayload[0] mismatches="<<mism_pay<<"/"<<payload_bytes.size()<<"\n";
    if(mism_pay==0) std::cout<<"[PASS] deinterleaver header+first payload parity"<<std::endl;
    return mism_pay==0 ? 0 : 1;
}
