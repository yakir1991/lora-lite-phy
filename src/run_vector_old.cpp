#include <iostream>
#include <fstream>
#include <vector>
#include <complex>
#include <cstring>
#include <algorithm>
#include <limits>
#include <cmath>
#include "receiver_lite.hpp"
#include "frame_sync_lite.hpp"
#include "fft_demod_lite.hpp"

using namespace lora_lite;

int main(int argc, char** argv){
    if(argc < 2){ std::cerr << "Usage: run_vector <file> [sf] [cr] [crc(0/1)] [implicit(0/1)] [ldro] [oversample]\n"; return 1; }
    const char* path = argv[1];
    uint8_t sf = (argc>2)? (uint8_t)std::atoi(argv[2]):7;
    uint8_t cr = (argc>3)? (uint8_t)std::atoi(argv[3]):2;
    bool has_crc = (argc>4)? (std::atoi(argv[4])!=0):true;
    bool implicit_hdr = (argc>5)? (std::atoi(argv[5])!=0):false;
    uint8_t ldro = (argc>6)? (uint8_t)std::atoi(argv[6]):0;
    uint8_t oversample = (argc>7)? (uint8_t)std::atoi(argv[7]):1;
    std::ifstream f(path, std::ios::binary); if(!f){ std::cerr << "Cannot open file: "<<path<<"\n"; return 1; }
    std::vector<float> buf((std::istreambuf_iterator<char>(f)), {}); // raw bytes
    if(buf.empty()){ std::cerr<<"Empty file or read error\n"; return 1; }
    size_t byte_count = buf.size();
    if(byte_count % sizeof(float)!=0){ std::cerr<<"File size not multiple of 4 bytes\n"; }
    size_t float_count = byte_count / sizeof(float);
    float* fptr = reinterpret_cast<float*>(buf.data());
    if(float_count % 2){ std::cerr<<"Float count not even (I/Q mismatch)\n"; return 1; }
    size_t complex_count = float_count / 2;
    std::vector<std::complex<float>> raw(complex_count);
    for(size_t i=0;i<complex_count;i++) raw[i] = { fptr[2*i], fptr[2*i+1] };
    size_t N = 1u << sf;
    size_t stride = static_cast<size_t>(N) * std::max<uint8_t>(1, oversample);
    if(stride == 0){ std::cerr<<"Invalid stride calculation"<<"\n"; return 1; }

    size_t header_symbol_index = 0;
    size_t header_sample_offset = 0;
    int cfo_int = 0;
    float cfo_frac = 0.0f;
    if(!implicit_hdr){
        auto sync = detect_preamble(raw, sf, oversample);
        if(sync.found){
            header_symbol_index = sync.header_symbol_index;
            header_sample_offset = sync.sample_offset;
            cfo_int = sync.cfo_int;
            cfo_frac = sync.cfo_frac;
            std::cout << "FrameSync metric=" << sync.metric
                      << " preamble_sample=" << sync.preamble_start
                      << " header_sample=" << sync.sample_offset
                      << " cfo_int=" << sync.cfo_int
                      << " cfo_frac=" << sync.cfo_frac
                      << " sto=" << sync.sto_frac << "\n";
        } else {
            std::cerr << "[warn] preamble not detected, defaulting to start of file" << "\n";
        }
    }

    size_t start_sample = implicit_hdr ? 0 : header_sample_offset;
    if (start_sample >= raw.size()) {
        start_sample = 0;
        header_symbol_index = 0;
    }

    size_t usable = (raw.size() - start_sample) / stride;
    if(usable == 0){ std::cerr<<"Unable to derive aligned symbols"<<"\n"; return 1; }
    std::vector<std::complex<float>> samples(usable * N);
    for(size_t s=0; s<usable; ++s){
        const std::complex<float>* base = raw.data() + start_sample + s*stride;
        if(oversample <= 1){
            std::copy(base, base + N, samples.begin() + s*N);
        } else {
            for(size_t k=0; k<N; ++k){
                std::complex<float> acc{0.f,0.f};
                for(uint8_t os=0; os<oversample; ++os){ acc += base[k*oversample + os]; }
                samples[s*N + k] = acc / static_cast<float>(oversample);
            }
        }
    }
    size_t symbols = usable;

    std::vector<uint16_t> coarse_syms(symbols);
    FftDemodLite coarse_fft(sf);
    for(size_t s=0; s<symbols; ++s){
        coarse_syms[s] = coarse_fft.demod(samples.data() + s*N);
    }

    if(!implicit_hdr){
        std::cout << "Coarse symbols (first 24 after collapse): ";
        for(size_t i=0; i<std::min<size_t>(24, symbols); ++i){
            std::cout << coarse_syms[i] << ' ';
        }
        std::cout << "\n";
    }

    ReceiverLite rx({sf,cr,has_crc,implicit_hdr,ldro});
    if(!implicit_hdr && (cfo_int != 0 || std::abs(cfo_frac) > 1e-6f)){
        rx.apply_cfo(cfo_int, cfo_frac);
    }

    const std::complex<float>* frame_start = samples.data();
    auto res = rx.decode(frame_start, symbols);

    std::cout << "Decoded: ok="<<res.ok<<" crc_ok="<<res.crc_ok<<" payload_bytes="<<res.payload.size()<<"\n";
    if(!res.payload.empty()){
        std::cout<<"Payload(hex): ";
        for(auto b: res.payload){ std::cout<<std::hex<<std::uppercase<<(int)b<<" "; }
        std::cout<<std::dec<<"\n";
    }
    if(!implicit_hdr){
        const auto& codewords = rx.last_header_codewords_raw();
        if(!codewords.empty()){
            std::cout<<"Header codewords: "; for(auto c: codewords) std::cout<<(int)c<<" "; std::cout<<"\n";
        }
    }
    return 0;
}
