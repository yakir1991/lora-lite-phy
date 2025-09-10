#pragma once
#include <cstdint>
#include <vector>
#include <complex>
#include <unordered_map>
#include <liquid/liquid.h>
#include "lora/utils/interleaver.hpp"
#include "lora/utils/hamming.hpp"

using liquid_fftplan = fftplan;

namespace lora {

struct Workspace {
    uint32_t sf{};
    uint32_t N{};
    liquid_fftplan plan{};
    std::vector<std::complex<float>> upchirp;
    std::vector<std::complex<float>> downchirp;
    std::vector<std::complex<float>> rxbuf;
    std::vector<std::complex<float>> fftbuf;

    // Scratch buffers for RX path to avoid per-call allocations
    std::vector<uint32_t> rx_symbols;
    std::vector<uint8_t>  rx_bits;
    std::vector<uint8_t>  rx_deint;
    std::vector<uint8_t>  rx_nibbles;
    std::vector<uint8_t>  rx_data;

    // Scratch buffers for TX path to avoid per-call allocations
    std::vector<uint8_t>  tx_bits;
    std::vector<uint8_t>  tx_inter;
    std::vector<uint32_t> tx_symbols;
    std::vector<std::complex<float>> tx_iq;

    // Cached interleaver maps indexed by (sf << 8) | cr_plus4
    std::unordered_map<uint32_t, lora::utils::InterleaverMap> interleavers;

    // Debug/instrumentation buffers for RX (filled optionally by decoder)
    std::vector<uint8_t> dbg_predew;   // Bytes after deinterleave+hamming (before dewhitening), len = payload_len+2
    std::vector<uint8_t> dbg_postdew;  // Bytes after dewhitening (payload dewhitened + raw CRC)
    uint16_t dbg_crc_calc{};           // CRC computed over payload bytes
    uint16_t dbg_crc_rx_le{};          // CRC trailer interpreted as LE
    uint16_t dbg_crc_rx_be{};          // CRC trailer interpreted as BE
    bool     dbg_crc_ok_le{};
    bool     dbg_crc_ok_be{};
    uint32_t dbg_payload_len{};        // Payload length inferred from header
    lora::utils::CodeRate dbg_cr_payload{lora::utils::CodeRate::CR45};

    // Header debug
    bool     dbg_hdr_filled{};         // set true when fields below are valid
    uint32_t dbg_hdr_sf{};
    // First 16 header symbols (raw peak bin, corrected, and gray-coded)
    uint32_t dbg_hdr_syms_raw[16]{};
    uint32_t dbg_hdr_syms_corr[16]{};
    uint32_t dbg_hdr_gray[16]{};
    // Hamming-decoded header nibbles for CR48 and CR45 attempts (10 nibbles each)
    uint8_t  dbg_hdr_nibbles_cr48[10]{};
    uint8_t  dbg_hdr_nibbles_cr45[10]{};

    ~Workspace();
    void init(uint32_t new_sf);
    void fft(const std::complex<float>* in, std::complex<float>* out);
    void ensure_rx_buffers(size_t nsym, uint32_t sf, uint32_t cr_plus4);
    void ensure_tx_buffers(size_t payload_len, uint32_t sf, uint32_t cr_plus4);
    const lora::utils::InterleaverMap& get_interleaver(uint32_t sf, uint32_t cr_plus4);
};

} // namespace lora

