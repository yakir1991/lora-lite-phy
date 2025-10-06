#include "frame_sync.hpp"
#include "iq_loader.hpp"
#include "sync_word_detector.hpp"

#include <filesystem>
#include <iostream>
#include <string>

// Developer utility to inspect the raw bins/magnitudes around the preamble and
// sync symbols. Helpful when tuning frame synchronization thresholds or verifying
// captures whose sync words differ from the default 0x12. The tool prints CSV-like
// sequences so they can be piped into quick plots or diffed between runs.

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <file.cf32>\n";
        return 2;
    }
    std::filesystem::path input = argv[1];
    int sf = 7;
    int bw = 125000;
    int fs = 500000;
    unsigned sync_word = 0x12u;

    try {
        auto samples = lora::IqLoader::load_cf32(input);
        lora::FrameSynchronizer fsync(sf, bw, fs);
        auto sync = fsync.synchronize(samples);
        if (!sync.has_value()) {
            std::cout << "sync=none\n";
            return 1;
        }
        lora::SyncWordDetector swd(sf, bw, fs, sync_word);
        auto det = swd.analyze(samples, sync->preamble_offset, sync->cfo_hz);
        if (!det.has_value()) {
            std::cout << "analyze=none\n";
            return 1;
        }
        std::cout << "bins=";
        for (size_t i = 0; i < det->symbol_bins.size(); ++i) {
            std::cout << det->symbol_bins[i];
            if (i + 1 < det->symbol_bins.size()) std::cout << ',';
        }
        std::cout << "\n";
        std::cout << "mags=";
        for (size_t i = 0; i < det->magnitudes.size(); ++i) {
            std::cout << det->magnitudes[i];
            if (i + 1 < det->magnitudes.size()) std::cout << ',';
        }
        std::cout << "\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 2;
    }
}
