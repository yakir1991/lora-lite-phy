#include "iq_loader.hpp"

#include <fstream>
#include <stdexcept>

namespace lora {

std::vector<IqLoader::Sample> IqLoader::load_cf32(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open IQ file: " + path.string());
    }

    file.seekg(0, std::ios::end);
    const std::streampos file_size = file.tellg();
    if (file_size % (sizeof(float) * 2) != 0) {
        throw std::runtime_error("IQ file size is not aligned to complex64 samples: " + path.string());
    }
    const std::size_t sample_count = static_cast<std::size_t>(file_size) / (sizeof(float) * 2);
    file.seekg(0, std::ios::beg);

    std::vector<Sample> samples(sample_count);
    file.read(reinterpret_cast<char *>(samples.data()), sample_count * sizeof(Sample));
    if (!file) {
        throw std::runtime_error("Failed to read IQ data from file: " + path.string());
    }

    return samples;
}

} // namespace lora
