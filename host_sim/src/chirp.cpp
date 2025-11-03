#include "host_sim/chirp.hpp"

#include <cmath>
#include <numbers>

namespace host_sim
{

namespace
{

constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

}

ChirpTables build_chirps_with_id(int sf, int oversample_factor, int id)
{
    const int n_bins = 1 << sf;
    const int samples_per_symbol = n_bins * oversample_factor;
    const int wrapped_id = ((id % n_bins) + n_bins) % n_bins;
    const float N = static_cast<float>(n_bins);
    const float os = static_cast<float>(oversample_factor);
    const int n_fold = samples_per_symbol - wrapped_id * oversample_factor;

    ChirpTables tables;
    tables.upchirp.resize(samples_per_symbol);
    tables.downchirp.resize(samples_per_symbol);

    for (int n = 0; n < samples_per_symbol; ++n) {
        const float n_f = static_cast<float>(n);
        const float quadratic = n_f * n_f / (2.0f * N * os * os);
        float linear = (static_cast<float>(wrapped_id) / N - 0.5f) * n_f / os;
        if (n >= n_fold) {
            linear = (static_cast<float>(wrapped_id) / N - 1.5f) * n_f / os;
        }
        const float phase = quadratic + linear;
        tables.upchirp[n] = std::polar(1.0f, kTwoPi * phase);
        tables.downchirp[n] = std::conj(tables.upchirp[n]);
    }
    return tables;
}

ChirpTables build_chirps(int sf, int oversample_factor)
{
    return build_chirps_with_id(sf, oversample_factor, 0);
}

ChirpTablesQ15 build_chirps_q15_with_id(int sf, int oversample_factor, int id)
{
    const auto float_tables = build_chirps_with_id(sf, oversample_factor, id);
    ChirpTablesQ15 tables;
    tables.upchirp.resize(float_tables.upchirp.size());
    tables.downchirp.resize(float_tables.downchirp.size());

    for (std::size_t i = 0; i < float_tables.upchirp.size(); ++i) {
        const auto& up = float_tables.upchirp[i];
        const auto& down = float_tables.downchirp[i];
        tables.upchirp[i] = float_to_q15_complex(up.real(), up.imag());
        tables.downchirp[i] = float_to_q15_complex(down.real(), down.imag());
    }

    return tables;
}

ChirpTablesQ15 build_chirps_q15(int sf, int oversample_factor)
{
    return build_chirps_q15_with_id(sf, oversample_factor, 0);
}

} // namespace host_sim
