// examples/minimal_rx.cpp — Decode a CF32 IQ file and print the payload.
//
// Build:
//   cmake -B build -G Ninja
//   cmake --build build --target example_rx
//
// Run:
//   ./build/examples/example_rx <capture.cf32> <metadata.json>
//
// This is a stripped-down version of lora_replay showing the essential
// decode pipeline: load → burst detect → align → demod → decode → print.

#include "host_sim/alignment.hpp"
#include "host_sim/capture.hpp"
#include "host_sim/deinterleaver.hpp"
#include "host_sim/fft_demod.hpp"
#include "host_sim/hamming.hpp"
#include "host_sim/lora_params.hpp"
#include "host_sim/whitening.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: " << argv[0] << " <capture.cf32> <metadata.json>\n";
        return EXIT_FAILURE;
    }

    // 1. Load IQ samples and metadata
    const auto samples = host_sim::load_cf32(argv[1]);
    const auto meta = host_sim::load_metadata(argv[2]);

    std::cout << "Loaded " << samples.size() << " samples"
              << " | SF=" << meta.sf << " BW=" << meta.bw
              << " CR=4/" << (4 + meta.cr) << "\n";

    // 2. Create demodulator
    host_sim::FftDemodulator demod(meta.sf, meta.sample_rate, meta.bw);
    const int sps = demod.samples_per_symbol();

    // 3. Burst detection — find where the packet starts
    const auto burst = host_sim::detect_burst_start(samples, sps);
    const std::size_t burst_offset = burst.value_or(0);
    std::cout << "Burst at sample " << burst_offset << "\n";

    // 4. Symbol alignment — find exact preamble boundary
    const std::size_t window = static_cast<std::size_t>(sps) *
                               static_cast<std::size_t>(meta.preamble_len + 6);
    const std::size_t view_len = std::min(samples.size() - burst_offset, window);
    std::vector<std::complex<float>> burst_view(
        samples.data() + burst_offset,
        samples.data() + burst_offset + view_len);

    const std::size_t alignment = burst_offset +
        host_sim::find_symbol_alignment(burst_view, demod, meta.preamble_len);
    std::cout << "Alignment at sample " << alignment << "\n";

    // 5. Demodulate all available symbols
    const std::size_t avail = (samples.size() - alignment) / static_cast<std::size_t>(sps);
    std::vector<uint16_t> symbols;
    for (std::size_t i = 0; i < avail; ++i)
    {
        const auto* ptr = samples.data() + alignment + i * sps;
        symbols.push_back(demod.demodulate(ptr));
    }
    std::cout << "Demodulated " << symbols.size() << " symbols\n";

    // 6. Header decode (first 8 symbols, SF bits reduced, CR=4/8)
    host_sim::DeinterleaverConfig hdr_cfg{meta.sf, 4, true, meta.ldro};
    std::vector<uint16_t> hdr_syms(symbols.begin(),
                                    symbols.begin() + std::min<std::size_t>(8, symbols.size()));
    std::size_t consumed = 0;
    auto hdr_cw = host_sim::deinterleave(hdr_syms, hdr_cfg, consumed);
    auto hdr_nib = host_sim::hamming_decode_block(hdr_cw, true, 4);

    if (hdr_nib.size() >= 5)
    {
        const int payload_len = ((hdr_nib[0] & 0xF) << 4) | (hdr_nib[1] & 0xF);
        const bool has_crc = (hdr_nib[2] & 0x1) != 0;
        const int cr = (hdr_nib[2] >> 1) & 0x7;
        std::cout << "Header: payload_len=" << payload_len
                  << " crc=" << has_crc << " cr=" << cr << "\n";

        // 7. Payload decode
        const int cw_len = cr + 4;
        host_sim::DeinterleaverConfig pay_cfg{meta.sf, cr, false, meta.ldro};
        std::vector<uint8_t> payload_nibbles;

        // Collect remaining header nibbles
        for (std::size_t i = 5; i < hdr_nib.size(); ++i)
        {
            payload_nibbles.push_back(hdr_nib[i]);
        }

        std::size_t cursor = consumed;
        const std::size_t target = static_cast<std::size_t>(payload_len) * 2 +
                                   (has_crc ? 4 : 0);
        while (cursor + cw_len <= symbols.size() && payload_nibbles.size() < target)
        {
            std::vector<uint16_t> blk(symbols.begin() + cursor,
                                       symbols.begin() + cursor + cw_len);
            std::size_t blk_consumed = 0;
            auto cw = host_sim::deinterleave(blk, pay_cfg, blk_consumed);
            if (blk_consumed == 0)
            {
                break;
            }
            cursor += blk_consumed;
            auto nibs = host_sim::hamming_decode_block(cw, false, cr);
            payload_nibbles.insert(payload_nibbles.end(), nibs.begin(), nibs.end());
        }

        // 8. De-whitening
        host_sim::WhiteningSequencer seq;
        auto whitening = seq.sequence(payload_nibbles.size() / 2);

        std::string payload_ascii;
        for (std::size_t i = 0; i + 1 < payload_nibbles.size() &&
             payload_ascii.size() < static_cast<std::size_t>(payload_len); i += 2)
        {
            const std::size_t byte_idx = i / 2;
            const uint8_t w = (byte_idx < whitening.size()) ? whitening[byte_idx] : 0;
            const uint8_t lo = (payload_nibbles[i] & 0xF) ^ (w & 0x0F);
            const uint8_t hi = (payload_nibbles[i + 1] & 0xF) ^ ((w >> 4) & 0x0F);
            const char ch = static_cast<char>((hi << 4) | lo);
            payload_ascii += ch;
        }

        std::cout << "\nDecoded payload: " << payload_ascii << "\n";
    }
    else
    {
        std::cerr << "Header decode failed (not enough nibbles)\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
