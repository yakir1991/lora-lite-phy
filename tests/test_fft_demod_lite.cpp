#include "fft_demod_lite.hpp"
#include <cassert>
#include <complex>
#include <vector>
#include <iostream>

// Simple synthetic test: generate a pure upchirp shifted by symbol id then verify demod returns id.
// This mirrors the dechirp+FFT argmax approach.

static std::vector<std::complex<float>> make_symbol(uint8_t sf, uint16_t sym) {
    uint32_t N = 1u << sf; std::vector<std::complex<float>> v(N);
    for (uint32_t n=0;n<N;n++) {
        // Upchirp with ID 'sym' (wrap by N)
        float id = float(sym % N);
        float phase = 2.0f*3.14159265358979323846f*( ( n*n /(2.0f*N) ) + (id/float(N)-0.5f)*n );
        v[n] = {cosf(phase), sinf(phase)};
    }
    return v;
}

int main(){
    for(uint8_t sf=7; sf<=9; ++sf){
        lora_lite::FftDemodLite d(sf);
        uint32_t N = 1u<<sf;
        for(uint16_t sym=0; sym< N; sym+= (N/8)) {
            auto s = make_symbol(sf, sym);
            auto got = d.demod(s.data());
            if(got != sym){
                std::cerr << "Mismatch sf=" << int(sf) << " sym="<< sym << " got="<<got <<"\n";
                return 1;
            }
        }
    }
    std::cout << "fft_demod_lite basic test OK\n";
    return 0;
}
