#include "receiver_lite.hpp"
#include "json_util.hpp"
#include "hamming.hpp"
#include <iostream>
#include <vector>
#include <complex>
#include <regex>
#include <filesystem>

using namespace lora_lite;
namespace fs = std::filesystem;

static bool load_fft_samples(const std::string& txt, std::vector<std::complex<float>>& out){
    auto pos = txt.find("\"fft_in_c\""); if(pos==std::string::npos) return false;
    auto pos_i = txt.find("\"i\"", pos); auto pos_q = txt.find("\"q\"", pos); if(pos_i==std::string::npos||pos_q==std::string::npos) return false;
    auto extract = [&](size_t start){ size_t lb=txt.find('[',start); if(lb==std::string::npos) return std::vector<float>{}; size_t p=lb+1; int d=1; while(p<txt.size()&&d>0){ if(txt[p]=='[') d++; else if(txt[p]==']') d--; ++p;} if(d) return std::vector<float>{}; std::string arr=txt.substr(lb+1,p-lb-2); std::regex re("(-?\\d+(?:\\.\\d+)?(?:e-?\\d+)?)"); std::vector<float> vals; for(auto it=std::sregex_iterator(arr.begin(),arr.end(),re); it!=std::sregex_iterator(); ++it) vals.push_back(std::stof((*it)[1])); return vals; };
    auto iv = extract(pos_i); auto qv = extract(pos_q); if(iv.size()!=qv.size()||iv.empty()) return false;
    out.resize(iv.size()); for(size_t k=0;k<iv.size();++k) out[k]={iv[k], qv[k]}; return true;
}

int main(){
    std::string rel = "stage_dump/tx_sf7_bw125000_cr2_crc1_impl0_ldro0_pay11_stage.json";
    std::string txt; if(!json_slurp(rel, txt)){
        if(!json_slurp("../"+rel, txt)){
            std::cerr<<"missing dump"; return 1;
        }
    }
    auto post_ref = json_extract_u8(txt, "post_hamming_bits_b");
    if(post_ref.empty()){
        auto ham_ref = json_extract_u8(txt, "hamming_b");
        if(ham_ref.empty()){ std::cerr<<"no oracle bits (no hamming_b)"; return 1; }
        post_ref.reserve(ham_ref.size()*4);
        for(uint8_t nib: ham_ref){ for(int b=0;b<4;b++) post_ref.push_back( (nib>>b)&1u ); }
        std::cout<<"Synthesized post_hamming_bits_b from hamming_b ("<<post_ref.size()<<" bits)\n";
    }
    // parse params from filename
    int sf=7, cr=2; bool crc=true; bool impl=false; int ldro=0; // fixed for this known vector
    std::vector<std::complex<float>> samples; if(!load_fft_samples(txt, samples)){ std::cerr<<"no samples"; return 1; }
    size_t N = 1u<<sf; if(samples.size()%N){ std::cerr<<"len mismatch"; return 1; }
    ReceiverLite rx({(uint8_t)sf,(uint8_t)cr,crc,impl,(uint8_t)ldro});
    auto res = rx.decode(samples.data(), samples.size()/N);
    auto got = rx.last_post_hamming_bits();
    auto raw_header_nibs = rx.last_header_nibbles_raw();
    auto raw_header_cw = rx.last_header_codewords_raw();
    auto syms_proc = rx.last_syms_proc();
    auto degray_vec = rx.last_degray();
    std::cout<<"Chosen header variant: offset="<<rx.last_header_offset()
             <<" norm_mode="<<rx.last_header_norm_mode()
             <<" divide_then_gray="<<rx.last_header_divide_then_gray()
             <<" linear_a="<<rx.last_header_linear_a()
             <<" linear_b="<<rx.last_header_linear_b()
             <<" score="<<rx.last_header_variant_score() <<"\n";
    std::cout<<"oracle_bits="<<post_ref.size()<<" got_bits="<<got.size()<<"\n";
    size_t mism=0, first_mismatch=SIZE_MAX; size_t cmp = std::min(post_ref.size(), got.size());
    for(size_t i=0;i<cmp;i++){ if(post_ref[i]!=got[i]){ if(first_mismatch==SIZE_MAX) first_mismatch=i; mism++; }}
    if(first_mismatch!=SIZE_MAX){
        size_t start = (first_mismatch>16)? first_mismatch-16:0; size_t end = std::min(first_mismatch+16, cmp);
        std::cout<<"First mismatch at bit "<<first_mismatch<<" (header_bits="<<( (sf-2)*4 )<<")"<<"\n";
        std::cout<<"ref : "; for(size_t i=start;i<end;i++) std::cout<<(int)post_ref[i]; std::cout<<"\n";
        std::cout<<"got : "; for(size_t i=start;i<end;i++) std::cout<<(i<got.size()?(int)got[i]:-1); std::cout<<"\n";
    }
    std::cout<<"Total mismatches="<<mism<<" cmp_len="<<cmp<<"\n";
    // Show first 32 header bits vs got
    size_t header_bits = (sf-2)*4; size_t show = std::min<size_t>(header_bits, 32);
    std::cout<<"Header ref: "; for(size_t i=0;i<show && i<post_ref.size();++i) std::cout<<(int)post_ref[i]; std::cout<<"\n";
    std::cout<<"Header got: "; for(size_t i=0;i<show && i<got.size();++i) std::cout<<(int)got[i]; std::cout<<"\n";
    auto ham_ref_full = json_extract_u8(txt, "hamming_b");
    if(!ham_ref_full.empty()){
        std::cout<<"Oracle header nibbles: "; for(size_t i=0;i<raw_header_nibs.size() && i<ham_ref_full.size();++i) std::cout<<(int)(ham_ref_full[i]&0xF)<<" "; std::cout<<"\n";
        std::cout<<"Raw decoded header nibbles: "; for(auto v: raw_header_nibs) std::cout<<(int)v<<" "; std::cout<<"\n";
        std::cout<<"Raw header codewords: "; for(auto v: raw_header_cw) std::cout<<(int)v<<" "; std::cout<<"\n";
        std::cout<<"Proc header syms (/4 applied): "; for(size_t i=0;i<8 && i<syms_proc.size(); ++i) std::cout<<syms_proc[i]<<" "; std::cout<<"\n";
        std::cout<<"Degray header syms: "; for(size_t i=0;i<8 && i<degray_vec.size(); ++i) std::cout<<degray_vec[i]<<" "; std::cout<<"\n";
        // Reconstruct header with mapping-search pathway for direct comparison of bit matrices
        int sf=7; int sf_app_header = sf-2; int header_cw_len=8;
        if(degray_vec.size()>=8){
            std::vector<uint8_t> in_bits(sf_app_header*header_cw_len);
            for(int col=0; col<header_cw_len; ++col){
                uint16_t sym = degray_vec[col];
                for(int b=0;b<sf_app_header;++b){
                    uint8_t bit = (sym>>b)&1u; int row = (sf_app_header-1-b); in_bits[col*sf_app_header + row] = bit;
                }
            }
            std::vector<uint8_t> out_bits(sf_app_header*header_cw_len);
            lora_lite::deinterleave_bits(in_bits.data(), out_bits.data(), sf_app_header, header_cw_len);
            // Dump row 0 bits for both our stored codeword and reconstructed path
            std::cout<<"Header row bit matrix (row 0): in cols -> ";
            for(int c=0;c<header_cw_len;c++){ std::cout<<(int)in_bits[c*sf_app_header+0]; }
            std::cout<<" | deintl -> ";
            for(int c=0;c<header_cw_len;c++){ std::cout<<(int)out_bits[0*header_cw_len + c]; }
            std::cout<<"\n";
            // Derive codewords from mapping path
            std::vector<uint8_t> map_codes(sf_app_header);
            for(int r=0;r<sf_app_header;r++){ uint32_t v=0; for(int c=0;c<header_cw_len;c++){ v=(v<<1)|(out_bits[r*header_cw_len + c]&1u);} map_codes[r]=(uint8_t)v; }
            std::cout<<"Map path codewords: "; for(auto v: map_codes) std::cout<<(int)v<<" "; std::cout<<"\n";
        }
    }
    // Additional header reconstruction brute-force mapping search
    auto gray_syms = json_extract_u16(txt, "gray_demap_sym");
    if(gray_syms.size()>=8){
        std::cout<<"Oracle gray_demap_sym header: "; for(int i=0;i<8;i++) std::cout<<gray_syms[i]<<" "; std::cout<<"\n";
    }
    auto ham_ref = json_extract_u8(txt, "hamming_b");
    if(gray_syms.size()>=8 && ham_ref.size()>=5){
        int sf_app_header = sf-2; int header_cw_len=8;
        // Expected header codewords by re-encoding oracle header nibbles (first sf-2 of ham_ref) at CR48
        lora_lite::HammingTables tables = lora_lite::build_hamming_tables();
        std::vector<uint8_t> header_nibbles(ham_ref.begin(), ham_ref.begin()+sf_app_header);
        auto rev4 = [](uint8_t v){ return (uint8_t)(((v&0x1)<<3)|((v&0x2)<<1)|((v&0x4)>>1)|((v&0x8)>>3)); };
        // Oracle nibble orientation uncertain; test both raw and reversed when forming expected codewords.
        std::vector<uint8_t> nibble_variants_flat; // store 2 * sf_app_header
        for(int i=0;i<sf_app_header;i++){ nibble_variants_flat.push_back(header_nibbles[i]); }
        for(int i=0;i<sf_app_header;i++){ nibble_variants_flat.push_back(rev4(header_nibbles[i])); }
        struct Result { bool row_inv; bool col_rev; bool bit_rev; bool rev_nib; size_t mism; std::vector<uint8_t> codes; } best{false,false,false,false, (size_t)-1,{}};
        for(bool row_inv: {false,true}){
            for(bool col_rev: {false,true}){
                for(bool bit_rev: {false,true}){
                    for(bool rev_nib: {false,true}){
                        std::vector<uint8_t> expected_codes; expected_codes.reserve(sf_app_header);
                        for(int i=0;i<sf_app_header;i++){
                            uint8_t nib = nibble_variants_flat[(rev_nib?sf_app_header:0)+i] & 0xF;
                            expected_codes.push_back(lora_lite::hamming_encode4(nib, lora_lite::CodeRate::CR48, tables));
                        }
                        // Build candidate codes from gray_syms mapping hypothesis
                        std::vector<uint8_t> candidate(sf_app_header,0);
                        // Prepare bit matrix in_bits[col * sf_app_header + row]
                        std::vector<uint8_t> in_bits(sf_app_header * header_cw_len);
                        for(int col=0; col<header_cw_len; ++col){
                            uint16_t sym = gray_syms[col];
                            for(int b=0;b<sf_app_header;b++){
                                uint8_t bit = (sym >> b) & 1u; int row = row_inv ? b : (sf_app_header-1-b); // choose orientation
                                in_bits[col*sf_app_header + row] = bit;
                            }
                        }
                        // deinterleave
                        std::vector<uint8_t> out_bits(sf_app_header * header_cw_len);
                        lora_lite::deinterleave_bits(in_bits.data(), out_bits.data(), sf_app_header, header_cw_len);
                        for(int r=0;r<sf_app_header;r++){
                            uint32_t v=0;
                            for(int c=0;c<header_cw_len;c++){
                                int cc = col_rev ? (header_cw_len-1-c):c;
                                v = (v<<1) | (out_bits[r*header_cw_len + cc] & 1u);
                            }
                            if(bit_rev){ // reverse 8 bits
                                uint8_t vv = (uint8_t)v; uint8_t rbits=0; for(int i=0;i<8;i++){ rbits = (uint8_t)((rbits<<1)|((vv>>i)&1u)); } v=rbits;
                            }
                            candidate[r] = (uint8_t)v;
                        }
                        size_t mism=0; for(int i=0;i<sf_app_header;i++){ if(candidate[i]!=expected_codes[i]) mism++; }
                        if(mism < best.mism){ best={row_inv,col_rev,bit_rev,rev_nib,mism,candidate}; }
                    }
                }
            }
        }
        std::cout<<"Header mapping search best mism="<<best.mism<<" of "<<sf_app_header<<" (row_inv="<<best.row_inv
                 <<" col_rev="<<best.col_rev<<" bit_rev="<<best.bit_rev<<" rev_nib="<<best.rev_nib<<")\n";
        std::cout<<"Best candidate codes:"; for(auto c: best.codes) std::cout<<" "<<(int)c; std::cout<<"\nExp codes:";
        // Recompute expected codes for chosen rev_nib flag
        std::vector<uint8_t> exp_codes; exp_codes.reserve(sf_app_header);
        for(int i=0;i<sf_app_header;i++){
            uint8_t nib = (best.rev_nib?rev4(header_nibbles[i]):header_nibbles[i]) & 0xF;
            exp_codes.push_back(lora_lite::hamming_encode4(nib, lora_lite::CodeRate::CR48, tables));
        }
        for(auto c: exp_codes) std::cout<<" "<<(int)c; std::cout<<"\n";
    }
    return 0;
}
