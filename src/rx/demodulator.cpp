#include "lora/rx/loopback_rx.hpp"
#include "lora/utils/whitening.hpp"
#include "lora/utils/gray.hpp"
#include "lora/utils/crc.hpp"
#include <cmath>
#include <chrono>
#include <fstream>

namespace lora::rx {

std::pair<std::span<uint8_t>, bool> loopback_rx(Workspace& ws,
                                                std::span<const std::complex<float>> samples,
                                                uint32_t sf,
                                                lora::utils::CodeRate cr,
                                                size_t payload_len,
                                                bool check_sync,
                                                uint8_t expected_sync) {
    auto start = std::chrono::steady_clock::now();
    auto log_time = [&]() {
        auto end = std::chrono::steady_clock::now();
        static std::ofstream log("../tests/perf/demod_profile.log", std::ios::app);
        log << "loopback_rx "
            << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()
            << "\n";
    };

    ws.init(sf);
    uint32_t N = ws.N;
    uint32_t cr_plus4 = static_cast<uint32_t>(cr) + 4;
    size_t nsym_total = samples.size() / N;
    size_t data_offset = 0;
    if (check_sync) {
        if (nsym_total == 0) {
            log_time();
            return {std::span<uint8_t>{}, false};
        }
        const std::complex<float>* block = samples.data();
        for (uint32_t n = 0; n < N; ++n)
            ws.rxbuf[n] = block[n] * ws.downchirp[n];
        ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
        uint32_t max_bin = 0;
        float max_mag = 0.f;
        for (uint32_t k = 0; k < N; ++k) {
            float mag = std::norm(ws.fftbuf[k]);
            if (mag > max_mag) {
                max_mag = mag;
                max_bin = k;
            }
        }
        uint32_t sync_sym = lora::utils::gray_decode(max_bin);
        if (sync_sym != expected_sync) {
            log_time();
            return {std::span<uint8_t>{}, false};
        }
        data_offset = 1;
        --nsym_total;
    }
    size_t nsym = nsym_total;
    ws.ensure_rx_buffers(nsym, sf, cr_plus4);

    // Demodulate symbols
    auto& symbols = ws.rx_symbols;
    for (size_t s_idx = 0; s_idx < nsym; ++s_idx) {
        const std::complex<float>* block = &samples[(s_idx + data_offset) * N];
        for (uint32_t n = 0; n < N; ++n)
            ws.rxbuf[n] = block[n] * ws.downchirp[n];
        ws.fft(ws.rxbuf.data(), ws.fftbuf.data());
        uint32_t max_bin = 0;
        float max_mag = 0.f;
        for (uint32_t k = 0; k < N; ++k) {
            float mag = std::norm(ws.fftbuf[k]);
            if (mag > max_mag) {
                max_mag = mag;
                max_bin = k;
            }
        }
        symbols[s_idx] = lora::utils::gray_decode(max_bin);
    }

    // Symbols -> bits
    auto& bits = ws.rx_bits;
    size_t bit_idx = 0;
    for (size_t s_idx = 0; s_idx < nsym; ++s_idx) {
        uint32_t sym = symbols[s_idx];
        for (uint32_t b = 0; b < sf; ++b)
            bits[bit_idx++] = (sym >> b) & 1;
    }
    size_t nbits = bit_idx;

    // Deinterleave
    const auto& M = ws.get_interleaver(sf, cr_plus4);
    auto& deint = ws.rx_deint;
    for (size_t off = 0; off < nbits; off += M.n_in) {
        for (uint32_t i = 0; i < M.n_out; ++i)
            deint[off + M.map[i]] = bits[off + i];
    }

    // Hamming decode
    static lora::utils::HammingTables T = lora::utils::make_hamming_tables();
    auto& nibbles = ws.rx_nibbles;
    size_t nib_idx = 0;
    for (size_t i = 0; i < nbits; i += cr_plus4) {
        uint16_t cw = 0;
        for (uint32_t b = 0; b < cr_plus4; ++b)
            cw = (cw << 1) | deint[i + b];
        auto dec = lora::utils::hamming_decode4(cw, cr_plus4, cr, T);
        if (!dec) {
            log_time();
            return {std::span<uint8_t>{}, false};
        }
        nibbles[nib_idx++] = dec->first & 0x0F;
    }

    // Combine nibbles -> bytes
    auto& data = ws.rx_data;
    size_t data_len = (nib_idx + 1) / 2;
    for (size_t i = 0; i < data_len; ++i) {
        uint8_t low  = (i * 2 < nib_idx) ? nibbles[i * 2] : 0;
        uint8_t high = (i * 2 + 1 < nib_idx) ? nibbles[i * 2 + 1] : 0;
        data[i] = (high << 4) | low;
    }
    size_t total_needed = payload_len + 2;
    if (data_len < total_needed) {
        log_time();
        return {std::span<uint8_t>{}, false};
    }

    // Dewhiten
    auto lfsr = lora::utils::LfsrWhitening::pn9_default();
    lfsr.apply(data.data(), total_needed);

    // CRC verify
    lora::utils::Crc16Ccitt crc16;
    auto ver = crc16.verify_with_trailer_be(data.data(), payload_len + 2);
    if (!ver.first) {
        log_time();
        return {std::span<uint8_t>{}, false};
    }
    log_time();
    return {std::span<uint8_t>(data.data(), payload_len), true};
}

} // namespace lora::rx

