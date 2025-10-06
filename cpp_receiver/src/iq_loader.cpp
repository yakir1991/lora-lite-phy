#include "iq_loader.hpp"

#include <fstream>
#include <stdexcept>

namespace lora {

// Load a binary stream of interleaved complex float32 IQ (cf32) into a vector of Sample.
// Assumptions:
//  - File contains little-endian float32 pairs [I0, Q0, I1, Q1, ...] with no header/footer.
//  - The Sample typedef in iq_loader.hpp matches two float32 (std::complex<float> or struct of 2 floats).
// Behavior:
//  - Verifies the file size is a multiple of sizeof(float)*2 (I+Q per sample).
//  - Reads the entire file into a contiguous buffer and returns it.
//  - Throws std::runtime_error on open/read/alignment failures.
std::vector<IqLoader::Sample> IqLoader::load_cf32(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open IQ file: " + path.string());
    }

    // Determine file size and validate expected cf32 interleaved layout.
    file.seekg(0, std::ios::end);
    const std::streampos file_size = file.tellg();
    if (file_size % (sizeof(float) * 2) != 0) {
        throw std::runtime_error("IQ file size is not aligned to complex64 samples: " + path.string());
    }
    const std::size_t sample_count = static_cast<std::size_t>(file_size) / (sizeof(float) * 2);
    file.seekg(0, std::ios::beg);

    // Read raw bytes directly into the Sample vector. Endianness is assumed little-endian host.
    std::vector<Sample> samples(sample_count);
    file.read(reinterpret_cast<char *>(samples.data()), sample_count * sizeof(Sample));
    if (!file) {
        throw std::runtime_error("Failed to read IQ data from file: " + path.string());
    }

    return samples;
}

} // namespace lora
