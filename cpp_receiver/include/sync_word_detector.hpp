#pragma once

#include <complex>
#include <cstddef>
#include <optional>
#include <vector>

namespace lora {

struct SyncWordDetection {
    // Coarse preamble start offset (samples) used for symbol indexing.
    std::size_t preamble_offset = 0;
    // Normalized K-domain bins for the 8 preamble symbols followed by 2 sync symbols.
    std::vector<int> symbol_bins;
    // Peak magnitudes per symbol (quality/debug metric).
    std::vector<double> magnitudes;
    // True if preamble bins normalize near DC within tolerance.
    bool preamble_ok = false;
    // True if both sync symbols match expected bins within tolerance.
    bool sync_ok = false;
};

class SyncWordDetector {
public:
    using Sample = std::complex<float>;

    // Construct with PHY parameters and the 8-bit LoRa sync word.
    SyncWordDetector(int sf, int bandwidth_hz, int sample_rate_hz, unsigned sync_word);

    // Analyze the two sync symbols following the preamble.
    // Inputs:
    //  - samples: entire I/Q capture
    //  - preamble_offset: sample index where preamble begins (from synchronizer)
    //  - cfo_hz: CFO estimate for rotation compensation (optional)
    // Returns SyncWordDetection with preamble_ok and sync_ok flags, or nullopt on failure.
    [[nodiscard]] std::optional<SyncWordDetection> analyze(const std::vector<Sample> &samples,
                                                           std::ptrdiff_t preamble_offset,
                                                           double cfo_hz = 0.0) const;

    // Samples per symbol for current SF/BW/Fs configuration.
    [[nodiscard]] std::size_t samples_per_symbol() const { return sps_; }

private:
    struct FFTScratch {
        // Workspace buffers reused to avoid reallocation during demodulation.
        std::vector<std::complex<double>> input;
        std::vector<std::complex<double>> spectrum;
    };

    // Demodulate a single symbol index starting from preamble_offset.
    // Applies CFO rotation, dechirps with a downchirp, decimates to K chips,
    // and performs a K-point IDFT-like search to find the dominant bin.
    // Returns the aligned K-domain bin and reports its magnitude.
    [[nodiscard]] std::size_t demod_symbol(const std::vector<Sample> &samples,
                                           std::size_t sym_index,
                                           std::ptrdiff_t preamble_offset,
                                           double cfo_hz,
                                           FFTScratch &scratch,
                                           double &magnitude) const;

    int sf_ = 7;
    int bandwidth_hz_ = 125000;
    int sample_rate_hz_ = 500000;
    unsigned sync_word_ = 0x12;
    std::size_t sps_ = 0;
    std::vector<std::complex<double>> downchirp_;
};

} // namespace lora
