#pragma once
#include <complex>
#include <cstdint>
#include <span>
#include <vector>
#include <optional>

namespace lora::standalone {

struct RxConfig {
    uint32_t sf{7};
    uint32_t bw{125000};
    uint32_t fs{250000};
    uint32_t os{2}; // fs/bw
    uint32_t preamble_min{8};
    bool debug_detection{false};
    bool capture_header_bits{false};
    bool capture_payload_blocks{false};
    bool trace_crc{false};
};

struct FrameOut {
    size_t start_sample{0};
    int os{1};
    int phase{0};
    std::vector<uint32_t> header_bins;   // typically 4 symbols
    std::vector<uint32_t> payload_bins;  // demo: fixed number or until buffer end
    std::vector<uint8_t> header_bits;    // raw bits after Gray demap (MSB-first per symbol)
    // Parsed header fields (explicit header)
    int payload_len{-1};
    int cr_idx{-1};
    bool has_crc{false};
    bool header_crc_ok{false};
    // Decoded payload results (when implemented)
    std::vector<uint8_t> payload_bytes;
    bool payload_crc_ok{false};
    // Diagnostics
    float cfo_fraction{0.f};
    int cfo_integer{0};
    bool sfd_found{false};
    size_t sfd_decim_pos{0};
    std::vector<uint8_t> header_interleaver_bits;
    std::vector<uint8_t> header_deinterleaver_bits;
    uint32_t header_rows{0};
    uint32_t header_cols{0};
    struct PayloadBlockBits {
        uint32_t rows{0};
        uint32_t cols{0};
        size_t symbol_offset{0};
        std::vector<uint8_t> inter_bits;   // column-major layout
        std::vector<uint8_t> deinter_bits; // row-major layout matching decode order
    };
    std::vector<PayloadBlockBits> payload_blocks_bits;
    std::vector<uint8_t> payload_bytes_raw;
    std::vector<uint8_t> payload_whitening_prns;
    struct PayloadTraceEntry {
        int index{0};
        uint8_t raw_byte{0};
        uint8_t whitening{0};
        uint8_t dewhitened{0};
        uint16_t crc_semtech_before{0};
        uint16_t crc_semtech_after{0};
        uint16_t crc_gr_before{0};
        uint16_t crc_gr_after{0};
        bool counted_semtech{false};
        bool counted_gr{false};
        bool is_crc_byte{false};
    };
    std::vector<PayloadTraceEntry> payload_crc_trace;
    uint16_t payload_crc_semtech{0};
    uint16_t payload_crc_gr{0};
    uint16_t payload_crc_rx{0};
    bool payload_crc_semtech_ok{false};
    bool payload_crc_gr_ok{false};
};

enum class RxState { SEARCH_PREAMBLE, LOCATE_HEADER, DEMOD_HEADER, DEMOD_PAYLOAD };

struct RxContext {
    RxConfig cfg;
    RxState state{RxState::SEARCH_PREAMBLE};
    size_t preamble_start_raw{0};
    int os{1};
    int phase{0};
    size_t cursor_raw{0};
    // derived
    uint32_t N{1u<<7};
    // working buffers
    std::vector<std::complex<float>> buffer; // raw IQ appended via feed()
};

class Receiver {
public:
    explicit Receiver(const RxConfig& cfg);
    // Feed more IQ samples (raw, at fs)
    void feed(std::span<const std::complex<float>> iq);
    // Run the state machine until it blocks; return any completed frames and clear them
    std::vector<FrameOut> run();
    // Convenience: one-shot offline processing
    std::optional<FrameOut> process(std::span<const std::complex<float>> iq);

private:
    RxContext ctx_;
    std::vector<std::complex<float>> up_;
    std::vector<std::complex<float>> down_;
    std::vector<FrameOut> ready_;
};

} // namespace lora::standalone
