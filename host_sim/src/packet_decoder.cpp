#include "host_sim/packet_decoder.hpp"

#include "host_sim/alignment.hpp"
#include "host_sim/fft_demod.hpp"
#include "host_sim/lora_replay/header_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace host_sim
{

namespace
{

/// Rough SNR: mean burst power against the noise ahead of the burst.
float estimate_snr_db(std::span<const std::complex<float>> samples, std::size_t burst_offset,
                      std::size_t alignment, int samples_per_symbol)
{
    const std::size_t noise_end = std::min<std::size_t>(burst_offset, 256);
    double noise = 0.0;
    for (std::size_t i = 0; i < noise_end; ++i)
    {
        noise += std::norm(samples[i]);
    }

    const std::size_t sig_end =
        std::min(samples.size(), alignment + static_cast<std::size_t>(samples_per_symbol) * 4);
    double signal = 0.0;
    std::size_t sig_n = 0;
    for (std::size_t i = alignment; i < sig_end; ++i)
    {
        signal += std::norm(samples[i]);
        ++sig_n;
    }

    if (noise_end == 0 || sig_n == 0)
    {
        return 0.0f;
    }
    const double noise_mean = noise / static_cast<double>(noise_end);
    const double signal_mean = signal / static_cast<double>(sig_n);
    if (noise_mean <= 0.0 || signal_mean <= noise_mean)
    {
        return 0.0f;
    }
    return static_cast<float>(10.0 * std::log10((signal_mean - noise_mean) / noise_mean));
}

} // namespace

std::optional<DecodedPacket> decode_packet(std::span<const std::complex<float>> samples,
                                           const LoRaMetadata& meta)
{
    FftDemodulator demod(meta.sf, meta.sample_rate, meta.bw);
    const int sps = demod.samples_per_symbol();
    if (sps <= 0 || samples.empty())
    {
        return std::nullopt;
    }

    const int preamble = meta.preamble_len > 0 ? meta.preamble_len : 8;

    // A capture that is already trimmed to the burst has no power step to find,
    // so fall back to the start of the buffer rather than giving up — the same
    // thing the replay tool does.
    const auto burst = detect_burst_start(samples.data(), samples.size(), sps);
    const std::size_t burst_offset = burst.value_or(0);

    // Align on the preamble. Give the search the preamble plus a few symbols of
    // sync word and header so it has something to lock onto.
    const std::size_t window =
        static_cast<std::size_t>(sps) * static_cast<std::size_t>(preamble + 6);
    const std::size_t available = samples.size() - burst_offset;
    if (available < window)
    {
        return std::nullopt;
    }
    // Align CFO-aware: real captures carry a carrier offset large enough that a
    // plain preamble correlation locks onto the wrong bin and the header then
    // fails to decode.
    const std::span<const std::complex<float>> burst_span(samples.data() + burst_offset, available);
    const auto search = find_symbol_alignment_cfo_aware(burst_span, demod, preamble);
    const std::size_t alignment = burst_offset + search.alignment_offset;
    if (alignment >= samples.size())
    {
        return std::nullopt;
    }

    // Estimate and apply the frequency offsets from the preamble, preferring the
    // integer bin the search found when it disagrees materially.
    {
        const std::size_t sym_avail = (samples.size() - alignment) / static_cast<std::size_t>(sps);
        const int usable = static_cast<int>(
            std::min<std::size_t>(sym_avail, static_cast<std::size_t>(std::max(preamble - 1, 0))));
        if (usable > 0)
        {
            auto est = demod.estimate_frequency_offsets(samples.data() + alignment, usable);
            if (search.preamble_bin != 0)
            {
                const int n_bins = 1 << meta.sf;
                int signed_bin = search.preamble_bin;
                if (signed_bin > n_bins / 2)
                {
                    signed_bin -= n_bins;
                }
                if (std::abs(signed_bin) > std::abs(est.cfo_int) + 2)
                {
                    est.cfo_int = signed_bin;
                }
            }
            demod.set_frequency_offsets(est.cfo_frac, est.cfo_int, est.sfo_slope);
        }
    }

    const std::size_t probe_count = (samples.size() - alignment) / static_cast<std::size_t>(sps);
    if (probe_count < 8)
    {
        return std::nullopt;
    }

    // First pass: locate the sync word so we know where the frame body starts.
    std::vector<uint16_t> probe;
    probe.reserve(probe_count);
    for (std::size_t i = 0; i < probe_count; ++i)
    {
        probe.push_back(
            demod.demodulate(samples.data() + alignment + i * static_cast<std::size_t>(sps)));
    }

    auto sync_pos = find_header_symbol_index(probe, meta.sync_word, meta.sf);
    if (!sync_pos)
    {
        sync_pos = static_cast<std::size_t>(preamble + 4);
    }

    // The LoRa SFD is 2.25 downchirps, so the data symbols begin a quarter
    // symbol past sync + 4. Re-demodulate from there, trying each quarter
    // offset until the header decodes — the same order the replay tool uses.
    const float cfo_frac = demod.current_cfo_frac();
    const int cfo_int = demod.current_cfo_int();
    const std::size_t quarter = static_cast<std::size_t>(sps) / 4;

    lora_replay::HeaderDecodeResult header;
    std::vector<uint16_t> symbols;
    std::size_t data_sample = alignment;

    for (int qoff : {1, 0, 2, 3})
    {
        const std::size_t candidate = alignment + *sync_pos * static_cast<std::size_t>(sps) +
                                      static_cast<std::size_t>(qoff) * quarter;
        if (candidate + 8ULL * static_cast<std::size_t>(sps) > samples.size())
        {
            continue;
        }

        demod.set_frequency_offsets(cfo_frac, cfo_int, 0.0f);
        demod.reset_symbol_counter();

        const std::size_t n = (samples.size() - candidate) / static_cast<std::size_t>(sps);
        std::vector<uint16_t> attempt;
        attempt.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            attempt.push_back(
                demod.demodulate(samples.data() + candidate + i * static_cast<std::size_t>(sps)));
        }

        auto candidate_header = lora_replay::try_decode_header(attempt, 0, meta);
        if (candidate_header.success)
        {
            header = candidate_header;
            symbols = std::move(attempt);
            data_sample = candidate;
            break;
        }
    }

    if (!header.success)
    {
        return std::nullopt;
    }

    const auto payload = lora_replay::decode_payload(symbols, header, meta);
    if (!payload.success)
    {
        return std::nullopt;
    }

    DecodedPacket out;
    out.payload = payload.payload;
    out.payload_len = header.payload_len > 0 ? header.payload_len : meta.payload_len;
    out.cr = header.cr > 0 ? header.cr : meta.cr;
    out.has_crc = payload.has_crc;
    out.crc_ok = payload.crc_ok;
    out.crc_received = payload.crc_received;
    out.crc_computed = payload.crc_computed;
    out.burst_offset = burst_offset;
    out.alignment = alignment;
    out.consumed = data_sample + payload.consumed_symbols * static_cast<std::size_t>(sps);
    out.snr_db = estimate_snr_db(samples, burst_offset, alignment, sps);
    return out;
}

} // namespace host_sim
