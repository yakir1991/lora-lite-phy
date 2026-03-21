#pragma once

#include "host_sim/live_source.hpp"

#include <atomic>
#include <cstddef>
#include <complex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace host_sim
{

struct SoapyStreamConfig
{
    std::string args;
    double sample_rate{0.0};
    double frequency{0.0};
    double bandwidth{0.0};
    double gain{0.0};
    std::size_t channel{0};
    std::size_t samples_per_symbol{0};
    std::size_t max_symbols_in_flight{64};
};

class SoapyLiveAdapter
{
public:
    SoapyLiveAdapter(LiveSymbolSource& sink, SoapyStreamConfig config);
    ~SoapyLiveAdapter();

    bool start();
    void stop();

private:
    bool running() const;
    bool initialise_device();
    void streaming_loop();

    LiveSymbolSource& sink_;
    SoapyStreamConfig config_;

#ifdef HOST_SIM_HAS_SOAPY
    std::shared_ptr<void> device_;
    void* stream_{nullptr};
#endif

    std::vector<std::complex<float>> buffer_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace host_sim
