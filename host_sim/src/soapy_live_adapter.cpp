#include "host_sim/soapy_live_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

#ifdef HOST_SIM_HAS_SOAPY
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Types.hpp>
#endif

namespace host_sim
{

SoapyLiveAdapter::SoapyLiveAdapter(LiveSymbolSource& sink, SoapyStreamConfig config)
    : sink_(sink),
      config_(std::move(config))
{
    if (config_.max_symbols_in_flight == 0) {
        config_.max_symbols_in_flight = 64;
    }
    if (config_.samples_per_symbol == 0) {
        config_.samples_per_symbol = 1;
    }
    buffer_.reserve(config_.samples_per_symbol * config_.max_symbols_in_flight);
}

SoapyLiveAdapter::~SoapyLiveAdapter()
{
    stop();
}

bool SoapyLiveAdapter::start()
{
    if (running()) {
        return true;
    }

#ifdef HOST_SIM_HAS_SOAPY
    if (!device_ && !initialise_device()) {
        return false;
    }

    running_.store(true);
    thread_ = std::thread(&SoapyLiveAdapter::streaming_loop, this);
    return true;
#else
    return false;
#endif
}

void SoapyLiveAdapter::stop()
{
    if (!running()) {
        return;
    }
    running_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }

#ifdef HOST_SIM_HAS_SOAPY
    if (device_) {
        auto dev = std::static_pointer_cast<SoapySDR::Device>(device_);
        if (stream_) {
            dev->deactivateStream(stream_, 0, 0);
            dev->closeStream(stream_);
            stream_ = nullptr;
        }
        dev.reset();
        device_.reset();
    }
#endif
}

bool SoapyLiveAdapter::running() const
{
    return running_.load(std::memory_order_acquire);
}

bool SoapyLiveAdapter::initialise_device()
{
#ifndef HOST_SIM_HAS_SOAPY
    return false;
#else
    SoapySDR::Kwargs args = SoapySDR::KwargsFromString(config_.args);
    auto deleter = [](SoapySDR::Device* dev) {
        SoapySDR::Device::unmake(dev);
    };
    std::shared_ptr<SoapySDR::Device> dev(SoapySDR::Device::make(args), deleter);
    if (!dev) {
        return false;
    }

    if (config_.sample_rate > 0.0) {
        dev->setSampleRate(SOAPY_SDR_RX, config_.channel, config_.sample_rate);
    }
    if (config_.bandwidth > 0.0) {
        dev->setBandwidth(SOAPY_SDR_RX, config_.channel, config_.bandwidth);
    }
    if (config_.frequency > 0.0) {
        dev->setFrequency(SOAPY_SDR_RX, config_.channel, config_.frequency);
    }
    if (config_.gain > 0.0) {
        dev->setGain(SOAPY_SDR_RX, config_.channel, config_.gain);
    }

    stream_ = dev->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, config_.channel);
    if (!stream_) {
        return false;
    }

    int status = dev->activateStream(stream_, 0, 0);
    if (status != 0) {
        dev->closeStream(stream_);
        stream_ = nullptr;
        return false;
    }

    device_ = dev;
    return true;
#endif
}

void SoapyLiveAdapter::streaming_loop()
{
#ifdef HOST_SIM_HAS_SOAPY
    auto dev = std::static_pointer_cast<SoapySDR::Device>(device_);
    const std::size_t symbol_size = config_.samples_per_symbol;
    std::vector<std::complex<float>> residue;

    buffer_.resize(symbol_size * config_.max_symbols_in_flight);

    while (running()) {
        void* buffs[] = {buffer_.data()};
        int flags = 0;
        long long time_ns = 0;
        int ret = dev->readStream(stream_, buffs, buffer_.size(), flags, time_ns, 100000);
        if (ret <= 0) {
            continue;
        }

        std::size_t available = static_cast<std::size_t>(ret);
        std::size_t offset = 0;

        if (!residue.empty()) {
            const std::size_t needed = symbol_size - residue.size();
            const std::size_t to_copy = std::min<std::size_t>(needed, available);
            residue.insert(residue.end(), buffer_.begin(), buffer_.begin() + to_copy);
            offset += to_copy;
            available -= to_copy;
            if (residue.size() == symbol_size) {
                host_sim::SymbolBuffer symbol;
                symbol.samples = residue;
                if (!sink_.push_symbol(std::move(symbol), std::chrono::milliseconds(10))) {
                    running_.store(false);
                    break;
                }
                residue.clear();
            }
        }

        while (available >= symbol_size) {
            host_sim::SymbolBuffer symbol;
            symbol.samples.insert(symbol.samples.end(),
                                  buffer_.begin() + static_cast<std::ptrdiff_t>(offset),
                                  buffer_.begin() + static_cast<std::ptrdiff_t>(offset + symbol_size));
            if (!sink_.push_symbol(std::move(symbol), std::chrono::milliseconds(10))) {
                running_.store(false);
                break;
            }
            offset += symbol_size;
            available -= symbol_size;
        }

        if (!running()) {
            break;
        }

        residue.insert(residue.end(),
                       buffer_.begin() + static_cast<std::ptrdiff_t>(offset),
                       buffer_.begin() + static_cast<std::ptrdiff_t>(offset + available));
    }

    if (!residue.empty()) {
        host_sim::SymbolBuffer symbol;
        symbol.samples = std::move(residue);
        if (!sink_.push_symbol(std::move(symbol), std::chrono::milliseconds(10))) {
            running_.store(false);
        }
    }
#endif
}

} // namespace host_sim
