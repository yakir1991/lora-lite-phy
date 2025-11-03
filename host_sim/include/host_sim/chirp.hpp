#pragma once

#include "host_sim/q15.hpp"

#include <complex>
#include <cstddef>
#include <vector>

namespace host_sim
{

struct ChirpTables
{
    std::vector<std::complex<float>> upchirp;
    std::vector<std::complex<float>> downchirp;
};

struct ChirpTablesQ15
{
    std::vector<Q15Complex> upchirp;
    std::vector<Q15Complex> downchirp;
};

ChirpTables build_chirps(int sf, int oversample_factor);
ChirpTables build_chirps_with_id(int sf, int oversample_factor, int id);
ChirpTablesQ15 build_chirps_q15(int sf, int oversample_factor);
ChirpTablesQ15 build_chirps_q15_with_id(int sf, int oversample_factor, int id);

}
