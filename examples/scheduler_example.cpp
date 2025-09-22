#include "lora/rx/scheduler.hpp"
#include <iostream>
#include <vector>
#include <complex>

using cfloat = std::complex<float>;

// Example: Process LoRa samples with different configurations
void process_lora_samples(const std::vector<cfloat>& samples, const RxConfig& cfg) {
    std::cout << "Processing " << samples.size() << " samples with:" << std::endl;
    std::cout << "  SF=" << (int)cfg.sf << std::endl;
    std::cout << "  OS=" << cfg.os << std::endl;
    std::cout << "  LDRO=" << (cfg.ldro ? "true" : "false") << std::endl;
    std::cout << "  CR=" << (int)cfg.cr_idx << std::endl;
    std::cout << "  Bandwidth=" << cfg.bandwidth_hz << " Hz" << std::endl;
    
    // Run the scheduler
    run_pipeline_offline(samples.data(), samples.size(), cfg);
}

int main() {
    std::cout << "=== LoRa Scheduler Example ===" << std::endl;
    
    // Create dummy IQ samples (in real usage, these would come from SDR or file)
    std::vector<cfloat> samples(10000);
    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = cfloat(0.1f * std::cos(i * 0.1f), 0.1f * std::sin(i * 0.1f));
    }
    
    // Example 1: SF=7, 125kHz
    std::cout << "\n--- Example 1: SF=7, 125kHz ---" << std::endl;
    RxConfig cfg1;
    cfg1.sf = 7;
    cfg1.os = 2;
    cfg1.ldro = false;
    cfg1.cr_idx = 1; // CR45
    cfg1.bandwidth_hz = 125000.0f;
    process_lora_samples(samples, cfg1);
    
    // Example 2: SF=8, 250kHz with LDRO
    std::cout << "\n--- Example 2: SF=8, 250kHz with LDRO ---" << std::endl;
    RxConfig cfg2;
    cfg2.sf = 8;
    cfg2.os = 4;
    cfg2.ldro = true;
    cfg2.cr_idx = 3; // CR47
    cfg2.bandwidth_hz = 250000.0f;
    process_lora_samples(samples, cfg2);
    
    // Example 3: SF=7, 500kHz
    std::cout << "\n--- Example 3: SF=7, 500kHz ---" << std::endl;
    RxConfig cfg3;
    cfg3.sf = 7;
    cfg3.os = 8;
    cfg3.ldro = false;
    cfg3.cr_idx = 3; // CR47
    cfg3.bandwidth_hz = 500000.0f;
    process_lora_samples(samples, cfg3);
    
    std::cout << "\n=== Example completed ===" << std::endl;
    return 0;
}
