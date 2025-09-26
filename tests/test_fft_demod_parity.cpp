// DRY version using shared JSON helpers
#include "fft_demod_lite.hpp"
#include "json_util.hpp"
#include <vector>
#include <complex>
#include <cstdint>
#include <iostream>
#include <regex>
#include <string>

// Minimal binary CF32 reader
static std::vector<std::complex<float>> read_cf32(const std::string& path){
    std::ifstream ifs(path, std::ios::binary);
    if(!ifs) throw std::runtime_error("cannot open file: "+path);
    ifs.seekg(0,std::ios::end); auto sz = ifs.tellg(); ifs.seekg(0);
    if(sz % (sizeof(float)*2) != 0) throw std::runtime_error("size not multiple of complex float");
    size_t n = static_cast<size_t>(sz) / (sizeof(float)*2);
    std::vector<std::complex<float>> v(n);
    for(size_t i=0;i<n;i++){ float re, im; ifs.read(reinterpret_cast<char*>(&re),4); ifs.read(reinterpret_cast<char*>(&im),4); v[i]={re,im}; }
    return v;
}

// (fft_demod_sym extracted via json_extract_u16)

// Parse frame_sync_c samples (already time-aligned & decimated to N per symbol) from JSON stage dump.
static std::vector<std::complex<float>> parse_frame_sync(const std::string& json_path){
    std::ifstream ifs(json_path); if(!ifs) throw std::runtime_error("cannot open json: "+json_path);
    std::string content((std::istreambuf_iterator<char>(ifs)),{});
    auto pos = content.find("\"frame_sync_c\"");
    if(pos==std::string::npos) throw std::runtime_error("frame_sync_c not found");
    auto pos_i = content.find("\"i\"", pos);
    auto pos_q = content.find("\"q\"", pos);
    if(pos_i==std::string::npos||pos_q==std::string::npos) throw std::runtime_error("i/q arrays missing");
    auto extract = [&](size_t start_key){
        size_t lb = content.find('[', start_key); if(lb==std::string::npos) throw std::runtime_error("array start not found");
        size_t end=lb+1; int depth=1; while(end<content.size() && depth>0){ if(content[end]=='[') depth++; else if(content[end]==']') depth--; ++end; }
        if(depth!=0) throw std::runtime_error("unterminated array");
        std::string arr=content.substr(lb+1, end-lb-2);
        std::vector<float> vals; vals.reserve(2048);
        std::regex num_re("(-?\\d+(?:\\.\\d+)?(?:e-?\\d+)?)");
        for(auto it=std::sregex_iterator(arr.begin(), arr.end(), num_re); it!=std::sregex_iterator(); ++it){ vals.push_back(std::stof((*it)[1])); }
        return vals;
    };
    auto i_vals = extract(pos_i);
    auto q_vals = extract(pos_q);
    if(i_vals.size()!=q_vals.size()) throw std::runtime_error("I/Q length mismatch");
    std::vector<std::complex<float>> out(i_vals.size());
    for(size_t k=0;k<i_vals.size();++k) out[k] = {i_vals[k], q_vals[k]};
    return out;
}

int main(){
    std::string dump_path = "stage_dump/tx_sf7_bw125000_cr2_crc1_impl0_ldro0_pay11_stage.json"; // existing dump
    if(!std::ifstream(dump_path)){
        dump_path = "../" + dump_path; // fallback when run from build dir
    }
    if(!std::ifstream(dump_path)){
        std::cerr<<"missing dump: "<<dump_path<<"\n"; return 1;
    }
    const uint8_t sf = 7; const size_t N = (1u<<sf);
    std::string content; if(!json_slurp(dump_path, content)){ std::cerr<<"cannot open json"; return 1; }
    auto gr_syms = json_extract_u16(content, "fft_demod_sym");
    if(gr_syms.empty()){ std::cerr << "No GR symbols parsed" << std::endl; return 1; }
    auto fs_samples = parse_frame_sync(dump_path);
    if(fs_samples.size() < N){ std::cerr << "frame_sync stream too short"<<std::endl; return 1; }

    // Infer oversampling (samples per symbol / N)
    size_t total = fs_samples.size();
    size_t os_guess = 1;
    if(!gr_syms.empty()){
        size_t per_sym = total / gr_syms.size();
        if(per_sym % N == 0) os_guess = per_sym / N;
    }
    if(os_guess==0) os_guess=1;
    size_t samples_per_symbol = N * os_guess;
    size_t sym_count = total / samples_per_symbol;
    std::cout << "frame_sync_c samples="<<total<<" N="<<N<<" inferred_os="<<os_guess<<" sym_count="<<sym_count<<" gr_syms="<<gr_syms.size()<<"\n";

    lora_lite::FftDemodLite dem(sf);
    std::vector<uint16_t> raw; raw.reserve(sym_count);
    for(size_t s=0;s<sym_count;s++){
        const std::complex<float>* base = &fs_samples[s*samples_per_symbol];
        raw.push_back(os_guess>1 ? dem.demod_oversampled(base, (uint8_t)os_guess) : dem.demod(base));
    }

    // Apply GR transformation per symbol: (idx-1) mod N then divide by 4 only for first 8 header symbols (no LDRO for SF7 @125k)
    const size_t header_symbols = 8;
    std::vector<uint16_t> ours_proc; ours_proc.reserve(raw.size());
    for(size_t i=0;i<raw.size();++i){
        uint16_t v = raw[i];
        uint16_t adj = (uint16_t)((v + N - 1) % N);
        if(i < header_symbols) adj = (uint16_t)(adj / 4); // header rate reduction
        ours_proc.push_back(adj);
    }

    size_t compare = std::min(ours_proc.size(), gr_syms.size());
    size_t leading=0; for(size_t i=0;i<compare;i++){ if(ours_proc[i]==gr_syms[i]) leading++; else break; }
    std::cout << "Leading matches="<<leading<<" / "<<compare<<" symbols"<<std::endl;
    if(leading == compare){
        std::cout << "[PASS] Full parity" << std::endl; return 0;
    }
    // Show first few diffs
    for(size_t i=0;i<std::min<size_t>(16, compare); ++i){
        std::cout<<i<<": gr="<<gr_syms[i]<<" ours="<<ours_proc[i]<<(ours_proc[i]==gr_syms[i]?"":" *")<<"\n";
    }
    return leading==compare ? 0 : 1;
}
