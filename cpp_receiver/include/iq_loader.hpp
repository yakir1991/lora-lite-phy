#pragma once

#include <complex>
#include <filesystem>
#include <string>
#include <vector>

namespace lora {

class IqLoader {
public:
    using Sample = std::complex<float>;

    // Load interleaved complex float32 (cf32) I/Q samples from disk.
    // File format:
    //  - Little-endian float32 pairs: [I0, Q0, I1, Q1, ...].
    //  - No headers/metadata are expected; raw binary stream only.
    // Behavior:
    //  - Reads the entire file into memory; throws on I/O errors.
    //  - If the file contains an odd number of float32 values, the last orphan value
    //    is ignored to maintain I/Q pairing.
    // Returns:
    //  - A vector of std::complex<float> with one element per I/Q pair.
    static std::vector<Sample> load_cf32(const std::filesystem::path &path);
};

} // namespace lora
