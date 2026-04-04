#pragma once

#include <complex>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace host_sim
{

std::vector<std::complex<float>> load_cf32(const std::filesystem::path& file_path);

/// Read complex-float32 IQ samples from stdin until EOF.
std::vector<std::complex<float>> load_cf32_stdin();

/// Read HackRF-native signed-int8 IQ from stdin and convert to complex float32.
std::vector<std::complex<float>> load_hackrf_stdin();

/// IQ format for streaming reader.
enum class IqFormat : uint8_t { cf32, hackrf_int8 };

/// Incremental stdin reader for real-time streaming decode.
/// Reads IQ in fixed-size chunks without waiting for EOF.
/// Uses offset-based consume for O(1) amortised discard.
class StreamingIqReader {
public:
    explicit StreamingIqReader(IqFormat format, std::size_t chunk_samples = 65536);

    /// Read one chunk from stdin.  Returns the number of new samples
    /// appended to the internal buffer.  Returns 0 on EOF.
    std::size_t read_chunk();

    /// True after stdin reaches EOF.
    bool eof() const { return eof_; }

    /// Total unconsumed samples in the buffer.
    std::size_t available() const { return buffer_.size() - offset_; }

    /// Pointer to the first unconsumed sample.
    const std::complex<float>* data() const { return buffer_.data() + offset_; }

    /// Discard the first @p n unconsumed samples.
    void consume(std::size_t n);

private:
    void compact();

    std::vector<std::complex<float>> buffer_;
    std::vector<int8_t> hackrf_scratch_;
    std::size_t offset_{0};
    IqFormat format_;
    std::size_t chunk_samples_;
    bool eof_{false};
    bool binary_set_{false};
};

struct CaptureStats
{
    std::size_t sample_count{0};
    float min_magnitude{0.0F};
    float max_magnitude{0.0F};
    float mean_power{0.0F};
};

CaptureStats analyse_capture(const std::vector<std::complex<float>>& samples);

} // namespace host_sim
