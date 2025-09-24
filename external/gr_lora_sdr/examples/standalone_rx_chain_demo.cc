#include <gnuradio/lora_sdr/standalone_rx_chain.h>

#include <complex>
#include <iomanip>
#include <iostream>
#include <vector>

int main()
{
    using namespace gr::lora_sdr::experimental;

    standalone_rx_chain::config cfg;
    cfg.fft_size = 256;
    cfg.window = window_block::window_type::hann;
    cfg.peak_threshold = 0.2F;
    cfg.relative_threshold = true;
    cfg.normalize_magnitude = true;

    standalone_rx_chain chain(cfg);

    std::vector<std::complex<float>> samples(cfg.fft_size);
    constexpr float pi = 3.14159265358979323846f;
    const float tone_bin = 42.0F;

    for (size_t n = 0; n < samples.size(); ++n) {
        const float phase = 2.0F * pi * tone_bin * static_cast<float>(n) /
                            static_cast<float>(cfg.fft_size);
        samples[n] = std::polar(1.0F, phase);
    }

    auto result = chain.process(samples);

    std::cout << "Processed " << result.input.size() << " samples" << std::endl;

    if (result.peak) {
        std::cout << "Dominant bin: " << result.peak->index << "\n"
                  << "Magnitude:   " << std::fixed << std::setprecision(3)
                  << result.peak->value << std::endl;
    } else {
        std::cout << "No peak above threshold" << std::endl;
    }

    return 0;
}
