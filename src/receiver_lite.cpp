#include "receiver_lite.hpp"
#include <algorithm>
#include <span>
#include <iostream>

namespace lora_lite {

namespace {
inline uint8_t rev4(uint8_t v) {
    return static_cast<uint8_t>(((v & 0x1) << 3) | ((v & 0x2) << 1) | ((v & 0x4) >> 1) | ((v & 0x8) >> 3));
}

inline uint16_t crc16_lora_style(std::span<const uint8_t> payload) {
    if (payload.size() < 2) return 0;
    uint16_t crc = 0;
    const size_t body_len = payload.size() - 2;
    for (size_t i = 0; i < body_len; ++i) {
        uint8_t new_byte = payload[i];
        for (int bit = 0; bit < 8; ++bit) {
            const bool feedback = (((crc & 0x8000u) >> 8) ^ (new_byte & 0x80u)) != 0;
            crc = static_cast<uint16_t>((crc << 1) & 0xFFFFu);
            if (feedback) crc = static_cast<uint16_t>(crc ^ 0x1021u);
            new_byte = static_cast<uint8_t>((new_byte << 1) & 0xFFu);
        }
    }
    crc ^= payload[payload.size() - 1];
    crc ^= static_cast<uint16_t>(payload[payload.size() - 2]) << 8;
    return static_cast<uint16_t>(crc & 0xFFFFu);
}
}

ReceiverLite::ReceiverLite(const RxParams& p) 
    : m_p(p), m_fft(p.sf), m_frame_sync(p.sf, p.oversample, p.sync_words), m_tables(build_hamming_tables()) {
    m_symbol_buffer.reserve(256); // Reserve space for symbols
}

RxResult ReceiverLite::process_samples(const std::vector<std::complex<float>>& samples) {
    RxResult result;
    
    switch (m_state) {
        case State::SYNC: {
            // Run frame synchronization
            auto sync_result = m_frame_sync.process_samples(samples);
            
            // Debug: Show sync progress
            static int debug_counter = 0;
            if (++debug_counter % 10 == 0) {
                std::cout << "[ReceiverLite] SYNC iteration " << debug_counter 
                          << ", samples=" << samples.size() 
                          << ", consumed=" << sync_result.samples_consumed
                          << ", frame_detected=" << sync_result.frame_detected << std::endl;
            }
            
            if (sync_result.frame_detected) {
                // Frame detected - extract sync parameters and transition to header decode
                result.frame_detected = true;
                result.cfo_int = sync_result.frame_info.cfo_int;
                result.cfo_frac = sync_result.frame_info.cfo_frac;
                result.snr_est = sync_result.snr_est;
                
                // Apply CFO correction to demod
                m_fft.apply_cfo(sync_result.frame_info.cfo_int, sync_result.frame_info.cfo_frac);
                
                // Transition to header decoding
                m_state = State::DECODE_HEADER;
                m_symbols_received = 0;
                m_symbols_needed = 8; // Header is always 8 symbols
                m_symbol_buffer.clear();
                
                std::cout << "[ReceiverLite] Frame detected! CFO: " << result.cfo_int 
                         << " + " << result.cfo_frac << ", SNR: " << result.snr_est << " dB" << std::endl;
            }
            
            if (sync_result.symbol_ready) {
                // This shouldn't happen in SYNC state, but handle gracefully
                std::cout << "[ReceiverLite] Unexpected symbol in SYNC state" << std::endl;
            }
            break;
        }
        
        case State::DECODE_HEADER: {
            auto sync_result = m_frame_sync.process_samples(samples);
            
            if (sync_result.symbol_ready) {
                // Accumulate header symbols
                const size_t N = 1u << m_p.sf;
                size_t current_size = m_symbol_buffer.size();
                m_symbol_buffer.resize(current_size + N);
                std::copy(sync_result.symbol_out.begin(), sync_result.symbol_out.end(), 
                         m_symbol_buffer.begin() + current_size);
                m_symbols_received++;
                
                if (m_symbols_received >= m_symbols_needed) {
                    // Header complete - decode it
                    auto decode_result = decode(m_symbol_buffer.data(), m_symbols_received);
                    
                    if (decode_result.ok || m_p.implicit_hdr) {
                        // Header decoded successfully or implicit header
                        if (!m_p.implicit_hdr) {
                            // Parse header to determine payload length
                            // For now, assume payload length is available from decode
                            // In a real implementation, you'd extract this from the header
                            FrameInfo frame_info;
                            frame_info.invalid_header = false;
                            frame_info.pay_len = 11; // Default payload length for testing
                            frame_info.has_crc = m_p.has_crc;
                            frame_info.cr = m_p.cr;
                            frame_info.ldro = (m_p.ldro == 2); // Auto-detect if ldro=2
                            
                            m_frame_sync.handle_frame_info(frame_info);
                        }
                        
                        // Transition to payload decoding
                        m_state = State::DECODE_PAYLOAD;
                        m_symbols_received = 0;
                        m_symbols_needed = 32; // Estimated payload symbols (will be updated)
                        m_symbol_buffer.clear();
                        
                        std::cout << "[ReceiverLite] Header decoded successfully" << std::endl;
                    } else {
                        // Header decode failed
                        std::cout << "[ReceiverLite] Header decode failed, returning to sync" << std::endl;
                        reset();
                    }
                }
            }
            break;
        }
        
        case State::DECODE_PAYLOAD: {
            auto sync_result = m_frame_sync.process_samples(samples);
            
            if (sync_result.symbol_ready) {
                // Accumulate payload symbols
                const size_t N = 1u << m_p.sf;
                size_t current_size = m_symbol_buffer.size();
                m_symbol_buffer.resize(current_size + N);
                std::copy(sync_result.symbol_out.begin(), sync_result.symbol_out.end(), 
                         m_symbol_buffer.begin() + current_size);
                m_symbols_received++;
                
                // Check if we have enough symbols (this is simplified)
                // In a real implementation, the exact count would come from header decode
                if (m_symbols_received >= 20) { // Estimated based on typical payload
                    // Payload complete - decode it
                    auto decode_result = decode(m_symbol_buffer.data(), m_symbols_received);
                    
                    result.ok = decode_result.ok;
                    result.crc_ok = decode_result.crc_ok;
                    result.payload = std::move(decode_result.payload);
                    
                    std::cout << "[ReceiverLite] Frame decode complete. Success: " << result.ok 
                             << ", CRC: " << result.crc_ok << ", Payload size: " << result.payload.size() << std::endl;
                    
                    // Return to sync for next frame
                    reset();
                }
            }
            break;
        }
    }
    
    return result;
}

void ReceiverLite::reset() {
    m_state = State::SYNC;
    m_symbols_received = 0;
    m_symbols_needed = 0;
    m_symbol_buffer.clear();
    m_frame_sync.reset();
}

void ReceiverLite::apply_cfo(int cfo_int, float cfo_frac) { 
    m_fft.apply_cfo(cfo_int, cfo_frac); 
}

RxResult ReceiverLite::decode(const std::complex<float>* samples, size_t symbol_count) {
    RxResult res;
    if (symbol_count == 0) return res;

    m_last_post_hamming_bits.clear();
    m_last_header_codewords_raw.clear();
    m_last_header_nibbles_raw.clear();
    m_last_syms_proc.clear();
    m_last_degray.clear();
    m_last_header_offset = 0;
    m_last_header_divide_then_gray = 0;
    m_last_header_norm_mode = 1;
    m_last_header_lin_a = 0;
    m_last_header_lin_b = 0;
    m_last_header_variant_score = 0;
    m_last_crc_calc = 0;
    m_last_crc_expected_lsb = 0;
    m_last_crc_expected_msb = 0;
    m_last_crc_observed_lsb = 0;
    m_last_crc_observed_msb = 0;
    m_last_crc_payload_len = 0;

    const uint32_t N = 1u << m_p.sf;
    std::vector<uint16_t> symbols_minus1;
    symbols_minus1.reserve(symbol_count);
    for (size_t s = 0; s < symbol_count; ++s) {
        uint16_t raw = m_fft.demod(samples + s * N);
        uint16_t norm = static_cast<uint16_t>((raw + N - 1) % N);
        symbols_minus1.push_back(norm);
    }
    m_last_syms_proc = symbols_minus1; // historical name (idx-1 values)
    m_last_degray = symbols_minus1;    // maintain previous debug expectation

    auto deinterleave_block = [&](size_t start, int cw_len, int sf_app, bool divide_by_4, std::vector<uint8_t>& out) -> bool {
        if (sf_app <= 0 || cw_len <= 0) return false;
        if (start + static_cast<size_t>(cw_len) > symbols_minus1.size()) return false;
        std::vector<uint8_t> in_bits(sf_app * cw_len);
        for (int col = 0; col < cw_len; ++col) {
            uint16_t sym_nat = symbols_minus1[start + static_cast<size_t>(col)];
            if (divide_by_4) sym_nat = static_cast<uint16_t>(sym_nat / 4u);
            uint16_t sym_gray = static_cast<uint16_t>(gray_encode(sym_nat));
            for (int b = 0; b < sf_app; ++b) {
                uint8_t bit = static_cast<uint8_t>((sym_gray >> b) & 1u);
                int row = sf_app - 1 - b;
                in_bits[col * sf_app + row] = bit;
            }
        }
        std::vector<uint8_t> out_bits(sf_app * cw_len);
        deinterleave_bits(in_bits.data(), out_bits.data(), sf_app, cw_len);
        out.resize(sf_app);
        for (int row = 0; row < sf_app; ++row) {
            uint32_t val = 0;
            for (int col = 0; col < cw_len; ++col) {
                val = (val << 1) | (out_bits[row * cw_len + col] & 1u);
            }
            out[row] = static_cast<uint8_t>(val & 0xFFu);
        }
        return true;
    };

    std::vector<uint8_t> data_bits;
    data_bits.reserve(symbol_count * 4);

    bool ldro_active = (m_p.ldro != 0);
    bool header_valid = true;
    size_t header_bits = 0;
    size_t payload_start_symbol = 0;
    uint8_t header_payload_len = 0;
    uint8_t header_cr_field = m_p.cr;
    bool header_crc_flag = m_p.has_crc;

    if (!m_p.implicit_hdr) {
        const int header_cols = 8;
        const int sf_app_header = std::max<int>(1, static_cast<int>(m_p.sf) - 2);
        if (symbol_count < static_cast<size_t>(header_cols)) return res;
        std::vector<uint8_t> header_codewords;
        if (!deinterleave_block(0, header_cols, sf_app_header, true, header_codewords)) return res;
        m_last_header_codewords_raw = header_codewords;

        for (uint8_t cw : header_codewords) {
            auto dec = hamming_decode4(cw, CodeRate::CR48, m_tables, true);
            if (!dec) {
                header_valid = false;
                break;
            }
            uint8_t nib = rev4(static_cast<uint8_t>(*dec & 0xF));
            m_last_header_nibbles_raw.push_back(nib);
            for (int b = 0; b < 4; ++b) data_bits.push_back((nib >> b) & 1u);
        }
        header_bits = m_last_header_nibbles_raw.size() * 4;
        if (header_valid && m_last_header_nibbles_raw.size() >= 5) {
            uint8_t n0 = m_last_header_nibbles_raw[0] & 0xF;
            uint8_t n1 = m_last_header_nibbles_raw[1] & 0xF;
            uint8_t n2 = m_last_header_nibbles_raw[2] & 0xF;
            uint8_t n3 = m_last_header_nibbles_raw[3] & 0xF;
            uint8_t n4 = m_last_header_nibbles_raw[4] & 0xF;
            header_payload_len = static_cast<uint8_t>((n0 << 4) | n1);
            header_crc_flag = (n2 & 0x1) != 0;
            header_cr_field = (n2 >> 1) & 0x7;

            uint8_t c4 = ((n0 >> 3) & 1u) ^ ((n0 >> 2) & 1u) ^ ((n0 >> 1) & 1u) ^ (n0 & 1u);
            uint8_t c3 = ((n0 >> 3) & 1u) ^ ((n1 >> 3) & 1u) ^ ((n1 >> 2) & 1u) ^ ((n1 >> 1) & 1u) ^ (n2 & 1u);
            uint8_t c2 = ((n0 >> 2) & 1u) ^ ((n1 >> 3) & 1u) ^ (n1 & 1u) ^ ((n2 >> 3) & 1u) ^ ((n2 >> 1) & 1u);
            uint8_t c1 = ((n0 >> 1) & 1u) ^ ((n1 >> 2) & 1u) ^ (n1 & 1u) ^ ((n2 >> 2) & 1u) ^ ((n2 >> 1) & 1u) ^ (n2 & 1u);
            uint8_t c0 = (n0 & 1u) ^ ((n1 >> 1) & 1u) ^ ((n2 >> 3) & 1u) ^ ((n2 >> 2) & 1u) ^ ((n2 >> 1) & 1u) ^ (n2 & 1u);
            uint8_t chk_expected = static_cast<uint8_t>((c4 << 4) | (c3 << 3) | (c2 << 2) | (c1 << 1) | c0);
            uint8_t chk_received = static_cast<uint8_t>(((n3 & 0x1u) << 4) | n4);
            if (header_payload_len == 0 || chk_expected != chk_received) header_valid = false;
        } else {
            header_valid = false;
        }

        if (!header_valid) return res;
        payload_start_symbol = header_cols;
    }

    uint8_t active_cr = m_p.implicit_hdr ? m_p.cr : header_cr_field;
    if (active_cr < 1 || active_cr > 4) active_cr = m_p.cr;
    bool crc_expected = m_p.implicit_hdr ? m_p.has_crc : header_crc_flag;
    uint8_t payload_len_header = m_p.implicit_hdr ? 0 : header_payload_len;

    int sf_app_payload = ldro_active ? std::max<int>(1, static_cast<int>(m_p.sf) - 2)
                                     : static_cast<int>(m_p.sf);
    const int cw_len_payload = 4 + static_cast<int>(active_cr);

    size_t sym_index = payload_start_symbol;
    while (sym_index + static_cast<size_t>(cw_len_payload) <= symbols_minus1.size()) {
        std::vector<uint8_t> cw_block;
        if (!deinterleave_block(sym_index, cw_len_payload, sf_app_payload, ldro_active, cw_block)) break;
        sym_index += static_cast<size_t>(cw_len_payload);

        for (int row = 0; row < sf_app_payload; ++row) {
            uint8_t cw = cw_block[row];
            auto dec = hamming_decode4(cw, map_cr(active_cr), m_tables, true);
            if (!dec) {
                header_valid = false;
                continue;
            }
            uint8_t nib = rev4(static_cast<uint8_t>(*dec & 0xF));
            for (int b = 0; b < 4; ++b) data_bits.push_back((nib >> b) & 1u);
        }

        if (!m_p.implicit_hdr && header_valid && payload_len_header > 0) {
            size_t needed_payload_bytes = payload_len_header + (crc_expected ? 2 : 0);
            size_t target_bits = header_bits + needed_payload_bytes * 8;
            if (data_bits.size() >= target_bits) break;
        }
    }

    if (!header_valid && !m_p.implicit_hdr) return res;
    if (data_bits.size() <= header_bits) return res;

    m_last_post_hamming_bits = data_bits;

    std::vector<uint8_t> payload_bits(data_bits.begin() + static_cast<long>(header_bits), data_bits.end());
    auto packed = pack_bits_lsb_first(payload_bits);
    if (packed.empty()) return res;

    if (!m_p.implicit_hdr && payload_len_header > 0) {
        size_t desired = payload_len_header + (crc_expected ? 2 : 0);
        if (packed.size() > desired) packed.resize(desired);
    }

    size_t payload_bytes_for_crc = 0;
    if (!m_p.implicit_hdr && payload_len_header > 0) {
        payload_bytes_for_crc = payload_len_header;
    } else if (crc_expected) {
        payload_bytes_for_crc = packed.size() >= 2 ? packed.size() - 2 : 0;
    } else {
        payload_bytes_for_crc = packed.size();
    }
    size_t whiten_prefix = std::min(packed.size(), payload_bytes_for_crc);
    auto dewhite = dewhiten_prefix(packed, whiten_prefix);
    size_t payload_bytes_span = std::min(payload_bytes_for_crc, dewhite.size());

    if (crc_expected) {
        if (payload_bytes_span < 2 || packed.size() < payload_bytes_span + 2) return res;
        auto payload_span = std::span<const uint8_t>(dewhite.data(), payload_bytes_span);
        uint16_t crc_calc = crc16_lora_style(payload_span);
        uint8_t crc_lsb = static_cast<uint8_t>(crc_calc & 0xFFu);
        uint8_t crc_msb = static_cast<uint8_t>((crc_calc >> 8) & 0xFFu);
        uint8_t expected_crc_lsb = crc_lsb;
        uint8_t expected_crc_msb = crc_msb;
        uint8_t observed_crc_lsb = packed[payload_bytes_span];
        uint8_t observed_crc_msb = packed[payload_bytes_span + 1];
        res.payload.assign(payload_span.begin(), payload_span.end());
        res.crc_ok = (expected_crc_lsb == observed_crc_lsb) && (expected_crc_msb == observed_crc_msb);
        res.ok = res.crc_ok;
        m_last_crc_calc = crc_calc;
        m_last_crc_expected_lsb = expected_crc_lsb;
        m_last_crc_expected_msb = expected_crc_msb;
        m_last_crc_observed_lsb = observed_crc_lsb;
        m_last_crc_observed_msb = observed_crc_msb;
        m_last_crc_payload_len = payload_bytes_span;
    } else {
        res.payload.assign(dewhite.begin(), dewhite.begin() + payload_bytes_span);
        res.ok = true;
        res.crc_ok = true;
        m_last_crc_payload_len = payload_bytes_span;
    }

    return res;
}

} // namespace lora_lite
