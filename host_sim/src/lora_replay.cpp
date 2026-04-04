#include "host_sim/alignment.hpp"
#include "host_sim/capture.hpp"
#include "host_sim/deinterleaver.hpp"
#include "host_sim/fft_demod.hpp"
#include "host_sim/fft_demod_ref.hpp"
#include "host_sim/hamming.hpp"
#include "host_sim/lora_params.hpp"
#include "host_sim/lora_replay/options.hpp"
#include "host_sim/lora_replay/stage_processing.hpp"
#include "host_sim/soft_decode.hpp"
#include "host_sim/scheduler.hpp"
#include "host_sim/stages/demod_stage.hpp"
#include "host_sim/whitening.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <cctype>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace
{

class FileSymbolSource : public host_sim::SymbolSource
{
public:
    FileSymbolSource(std::vector<std::complex<float>> samples,
                     std::size_t alignment_offset,
                     std::size_t samples_per_symbol,
                     std::size_t symbol_count)
        : samples_(std::move(samples)),
          offset_(alignment_offset),
          samples_per_symbol_(samples_per_symbol),
          symbol_count_(symbol_count)
    {
    }

    void reset() override { index_ = 0; }

    std::optional<host_sim::SymbolBuffer> next_symbol() override
    {
        if (index_ >= symbol_count_) {
            return std::nullopt;
        }
        const auto start = offset_ + index_ * samples_per_symbol_;
        if (start + samples_per_symbol_ > samples_.size()) {
            return std::nullopt;
        }
        host_sim::SymbolBuffer buffer;
        buffer.samples.insert(
            buffer.samples.end(),
            samples_.begin() + static_cast<std::ptrdiff_t>(start),
            samples_.begin() + static_cast<std::ptrdiff_t>(start + samples_per_symbol_));
        ++index_;
        return buffer;
    }

private:
    std::vector<std::complex<float>> samples_;
    std::size_t offset_;
    std::size_t samples_per_symbol_;
    std::size_t symbol_count_;
    std::size_t index_{0};
};

// Local CRC-16/CCITT: computes CRC over ALL input bytes (no XOR with trailing bytes).
// Different from host_sim::lora_replay::compute_lora_crc which includes the
// gr-lora_sdr last-2-byte XOR.  Used by probe_payload_crc which does the XOR manually.
uint16_t compute_raw_crc16(const std::vector<uint8_t>& payload)
{
    uint16_t crc = 0x0000;
    for (uint8_t byte : payload) {
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

std::size_t compute_samples_per_symbol(const host_sim::LoRaMetadata& meta)
{
    const std::size_t chips = static_cast<std::size_t>(1) << meta.sf;
    return static_cast<std::size_t>((static_cast<long long>(meta.sample_rate) * chips) / meta.bw);
}

struct InstrumentationResult
{
    std::vector<double> stage_timings_ns;
    std::vector<std::size_t> symbol_memory_bytes;
};

InstrumentationResult run_scheduler_instrumentation(const std::vector<std::complex<float>>& samples,
                                                    const host_sim::LoRaMetadata& meta,
                                                    std::size_t alignment_offset,
                                                    std::size_t max_symbols)
{
    InstrumentationResult result;
    host_sim::Scheduler scheduler;
    scheduler.configure({meta.sf, meta.bw, meta.sample_rate});

    auto demod_stage = std::make_shared<host_sim::DemodStage>();
    scheduler.attach_stage(demod_stage);

    scheduler.set_instrumentation({&result.stage_timings_ns, &result.symbol_memory_bytes});

    const std::size_t samples_per_symbol = compute_samples_per_symbol(meta);
    const std::size_t available_symbols =
        (samples.size() > alignment_offset)
            ? (samples.size() - alignment_offset) / samples_per_symbol
            : 0;
    const std::size_t symbol_count = std::min<std::size_t>(available_symbols, max_symbols);

    FileSymbolSource source(samples, alignment_offset, samples_per_symbol, symbol_count);
    scheduler.run(source);

    return result;
}

using host_sim::lora_replay::Options;
using host_sim::lora_replay::parse_arguments;
using host_sim::lora_replay::HeaderDecodeResult;
using host_sim::lora_replay::StageOutputs;
using host_sim::lora_replay::StageComparisonResult;
using host_sim::lora_replay::SummaryReport;
using host_sim::lora_replay::normalize_fft_symbol;
using host_sim::lora_replay::append_fft_gray;
using host_sim::lora_replay::read_stage_file;
using host_sim::lora_replay::write_stage_file;
using host_sim::lora_replay::compare_stage;
using host_sim::lora_replay::build_stage_summary_token;
using host_sim::lora_replay::write_summary_json;
using host_sim::lora_replay::compare_with_reference;
using host_sim::lora_replay::compute_lora_crc;

void write_stats_json(const std::filesystem::path& path,
                      const host_sim::CaptureStats& stats,
                      const Options& opts)
{
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Unable to open stats output file: " + path.string());
    }

    out << "{\n"
        << "  \"iq_file\": \"" << opts.iq_file.generic_string() << "\",\n"
        << "  \"sample_count\": " << stats.sample_count << ",\n"
        << "  \"min_magnitude\": " << std::setprecision(6) << stats.min_magnitude << ",\n"
        << "  \"max_magnitude\": " << std::setprecision(6) << stats.max_magnitude << ",\n"
        << "  \"mean_power\": " << std::setprecision(6) << stats.mean_power << "\n"
        << "}\n";
}

HeaderDecodeResult try_decode_header(const std::vector<uint16_t>& symbols,
                                     std::size_t start,
                                     const host_sim::LoRaMetadata& meta)
{
    static const bool debug_header = (std::getenv("HOST_SIM_DEBUG_HEADER") != nullptr);
    HeaderDecodeResult result;
    if (start + 8 > symbols.size()) {
        return result;
    }

    host_sim::DeinterleaverConfig header_cfg{meta.sf, 4, true, meta.ldro};
    const int block_symbols = 8;
    std::size_t cursor = start;
    std::size_t total_consumed = 0;
    std::vector<uint8_t> header_nibbles;
    std::vector<uint16_t> header_codewords;
    while (header_nibbles.size() < 5 && cursor + block_symbols <= symbols.size()) {
        std::vector<uint16_t> header_input(symbols.begin() + cursor,
                                           symbols.begin() + cursor + block_symbols);
        if (debug_header) {
            std::cout << "Header symbols (start=" << cursor << "):";
            for (auto value : header_input) {
                std::cout << ' ' << value;
            }
            std::cout << "\n";
        }

        std::size_t consumed_block = 0;
        auto codewords = host_sim::deinterleave(header_input, header_cfg, consumed_block);
        if (consumed_block == 0) {
            break;
        }
        total_consumed += consumed_block;
        cursor += consumed_block;
        header_codewords.insert(header_codewords.end(), codewords.begin(), codewords.end());

        if (debug_header) {
            std::cout << "Deinterleaved codewords:";
            for (auto cw : codewords) {
                std::cout << ' ' << std::hex << static_cast<int>(cw) << std::dec;
            }
            std::cout << "\n";
        }

        auto nibbles = host_sim::hamming_decode_block(codewords, true, 4);
        if (debug_header) {
            std::cout << "Header nibbles:";
            for (auto nib : nibbles) {
                std::cout << ' ' << std::hex << static_cast<int>(nib & 0xF) << std::dec;
            }
            std::cout << "\n";
        }
        header_nibbles.insert(header_nibbles.end(), nibbles.begin(), nibbles.end());
    }

    if (header_nibbles.size() < 5) {
        result.codewords = std::move(header_codewords);
        result.nibbles = header_nibbles;
        result.consumed_symbols = total_consumed;
        return result;
    }

    const int n0 = header_nibbles[0] & 0xF;
    const int n1 = header_nibbles[1] & 0xF;
    const int n2 = header_nibbles[2] & 0xF;
    const int n3 = header_nibbles[3] & 0xF;
    const int n4 = header_nibbles[4] & 0xF;

    const int payload_len = (n0 << 4) | n1;
    const bool has_crc = (n2 & 0x1) != 0;
    const int cr = (n2 >> 1) & 0x7;
    const int header_chk = ((n3 & 0x1) << 4) | n4;

    const bool c4 = ((n0 & 0x8) >> 3) ^ ((n0 & 0x4) >> 2) ^ ((n0 & 0x2) >> 1) ^ (n0 & 0x1);
    const bool c3 = ((n0 & 0x8) >> 3) ^ ((n1 & 0x8) >> 3) ^ ((n1 & 0x4) >> 2) ^ ((n1 & 0x2) >> 1) ^ (n2 & 0x1);
    const bool c2 = ((n0 & 0x4) >> 2) ^ ((n1 & 0x8) >> 3) ^ (n1 & 0x1) ^ ((n2 & 0x8) >> 3) ^ ((n2 & 0x2) >> 1);
    const bool c1 = ((n0 & 0x2) >> 1) ^ ((n1 & 0x4) >> 2) ^ (n1 & 0x1) ^ ((n2 & 0x4) >> 2) ^ ((n2 & 0x2) >> 1) ^ (n2 & 0x1);
    const bool c0 = (n0 & 0x1) ^ ((n1 & 0x2) >> 1) ^ ((n2 & 0x8) >> 3) ^ ((n2 & 0x4) >> 2) ^ ((n2 & 0x2) >> 1) ^ (n2 & 0x1);
    const int computed_checksum = (static_cast<int>(c4) << 4) | (static_cast<int>(c3) << 3) |
                                  (static_cast<int>(c2) << 2) | (static_cast<int>(c1) << 1) |
                                  static_cast<int>(c0);

    result.checksum_field = header_chk;
    result.checksum_computed = computed_checksum;

    if (payload_len <= 0 || header_chk != computed_checksum) {
        result.nibbles = header_nibbles;
        result.payload_len = payload_len;
        result.has_crc = has_crc;
        result.cr = cr;
        return result;
    }

    result.success = true;
    result.payload_len = payload_len;
    result.has_crc = has_crc;
    result.cr = cr;
    result.checksum_field = header_chk;
    result.checksum_computed = computed_checksum;
    result.consumed_symbols = total_consumed;
    result.codewords = std::move(header_codewords);
    result.nibbles = std::move(header_nibbles);
    return result;
}

// Upsample complex IQ data by 2x using linear interpolation.
// Doubles the effective sample rate so the demodulator gets finer
// timing resolution, extending SFO tolerance for long payloads.
std::vector<std::complex<float>> upsample_2x(
    const std::complex<float>* data, std::size_t count)
{
    if (count < 2) {
        return count == 1
            ? std::vector<std::complex<float>>{data[0]}
            : std::vector<std::complex<float>>{};
    }
    std::vector<std::complex<float>> out(count * 2 - 1);
    for (std::size_t i = 0; i < count - 1; ++i) {
        out[2 * i] = data[i];
        out[2 * i + 1] = 0.5f * (data[i] + data[i + 1]);
    }
    out[2 * (count - 1)] = data[count - 1];
    return out;
}

// Quick CRC probe: decode payload from symbols and check CRC.
// Used for data-start timing refinement at low oversampling,
// where SFO-induced timing drift can shift data symbols by ±1 bin.
bool probe_payload_crc(const std::vector<uint16_t>& symbols,
                       const HeaderDecodeResult& hdr,
                       const host_sim::LoRaMetadata& meta)
{
    if (!hdr.success) return false;
    const int pl = hdr.payload_len > 0 ? hdr.payload_len : meta.payload_len;
    const int cr = hdr.cr > 0 ? hdr.cr : meta.cr;
    const bool has_crc = hdr.has_crc || meta.has_crc;
    if (!has_crc || pl < 3) return false;

    std::vector<uint8_t> payload_nibbles;
    if (hdr.nibbles.size() > 5) {
        for (std::size_t i = 5; i < hdr.nibbles.size(); ++i) {
            payload_nibbles.push_back(hdr.nibbles[i]);
        }
    }

    const std::size_t nibble_target =
        static_cast<std::size_t>(pl) * 2 + 4;
    const int cw_len = cr + 4;
    std::size_t cursor = hdr.consumed_symbols > 0 ? hdr.consumed_symbols : 8;
    host_sim::DeinterleaverConfig payload_cfg{meta.sf, cr, false, meta.ldro};

    while (cursor + static_cast<std::size_t>(cw_len) <= symbols.size() &&
           payload_nibbles.size() < nibble_target) {
        std::vector<uint16_t> block(symbols.begin() + static_cast<std::ptrdiff_t>(cursor),
                                    symbols.begin() + static_cast<std::ptrdiff_t>(cursor + cw_len));
        std::size_t consumed = 0;
        auto codewords = host_sim::deinterleave(block, payload_cfg, consumed);
        if (consumed == 0) break;
        cursor += consumed;
        auto nibs = host_sim::hamming_decode_block(codewords, false, cr);
        payload_nibbles.insert(payload_nibbles.end(), nibs.begin(), nibs.end());
    }

    if (payload_nibbles.size() < static_cast<std::size_t>(pl) * 2 + 4)
        return false;

    host_sim::WhiteningSequencer seq;
    auto whitening = seq.sequence(payload_nibbles.size() / 2);

    std::vector<uint8_t> unwhitened;
    for (std::size_t i = 0; i + 1 < payload_nibbles.size() &&
         unwhitened.size() < static_cast<std::size_t>(pl) + 2; i += 2) {
        const std::size_t byte_idx = i / 2;
        uint8_t lo, hi;
        if (byte_idx < static_cast<std::size_t>(pl)) {
            const uint8_t w = (byte_idx < whitening.size()) ? whitening[byte_idx] : 0;
            lo = (payload_nibbles[i] & 0xF) ^ (w & 0x0F);
            hi = (payload_nibbles[i + 1] & 0xF) ^ ((w >> 4) & 0x0F);
        } else {
            lo = payload_nibbles[i] & 0xF;
            hi = payload_nibbles[i + 1] & 0xF;
        }
        unwhitened.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }

    if (unwhitened.size() < static_cast<std::size_t>(pl) + 2)
        return false;

    std::vector<uint8_t> crc_data(unwhitened.begin(),
                                  unwhitened.begin() + pl - 2);
    uint16_t computed = compute_raw_crc16(crc_data);
    if (pl >= 2) {
        computed ^= unwhitened[pl - 1];
        computed ^= static_cast<uint16_t>(unwhitened[pl - 2]) << 8;
    }
    const uint16_t decoded = static_cast<uint16_t>(unwhitened[pl]) |
                             (static_cast<uint16_t>(unwhitened[pl + 1]) << 8);
    return computed == decoded;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parse_arguments(argc, argv);
        if (options.verbose) std::cerr << "[debug] entering lora_replay" << std::endl;

        // ── Streaming mode ──────────────────────────────────────
        // Reads stdin incrementally.  Uses the same burst detection
        // (quartile noise floor) and decode pipeline as batch mode,
        // but processes each burst immediately so first-packet
        // latency is sub-second instead of waiting for stdin EOF.
        if (options.stream) {
            if (!options.metadata) {
                throw std::runtime_error("--stream requires --metadata");
            }
            const auto base_meta = host_sim::load_metadata(*options.metadata);
            const auto iq_fmt = (options.iq_format == Options::IqFormat::hackrf)
                                    ? host_sim::IqFormat::hackrf_int8
                                    : host_sim::IqFormat::cf32;

            // Chunk: ~100 ms of samples per read call
            const std::size_t chunk_samples =
                std::max<std::size_t>(4096, static_cast<std::size_t>(base_meta.sample_rate * 0.1));
            host_sim::StreamingIqReader reader(iq_fmt, chunk_samples);

            // Multi-SF demodulator bank (single entry when --multi-sf not set).
            struct SfCtx {
                std::unique_ptr<host_sim::FftDemodulator> demod;
                int sps, os;
            };
            std::vector<SfCtx> sf_bank;
            {
                const int sf_lo = options.multi_sf ? 6 : base_meta.sf;
                const int sf_hi = options.multi_sf ? 12 : base_meta.sf;
                for (int sf_i = sf_lo; sf_i <= sf_hi; ++sf_i) {
                    SfCtx ctx;
                    ctx.demod = std::make_unique<host_sim::FftDemodulator>(
                        sf_i, base_meta.sample_rate, base_meta.bw);
                    ctx.sps = ctx.demod->samples_per_symbol();
                    ctx.os = ctx.demod->oversample_factor();
                    sf_bank.push_back(std::move(ctx));
                }
            }
            // Accumulate enough data for the largest SF.
            const int max_sps = sf_bank.back().sps;
            const std::size_t min_accumulate =
                static_cast<std::size_t>(max_sps) * 60;

            if (options.multi_sf) {
                std::cout << "Multi-SF: SF6–SF12, BW=" << base_meta.bw
                          << ", Fs=" << base_meta.sample_rate << "\n";
            } else {
                std::cout << "Metadata: SF=" << base_meta.sf
                          << ", CR=" << base_meta.cr
                          << ", BW=" << base_meta.bw
                          << ", Fs=" << base_meta.sample_rate
                          << ", payload_len=" << base_meta.payload_len << "\n";
            }
            std::cout << "[stream] Listening... (max_sps=" << max_sps
                      << ", symbol_period="
                      << static_cast<double>(max_sps) / base_meta.sample_rate * 1000.0
                      << " ms)\n";
            std::cout.flush();

            std::size_t search_offset = 0;
            int packet_index = 0;
            float tracked_noise_floor = 0.0f;  // Adaptive noise estimate

            // PER/BER statistics counters (active when --per-stats)
            int stat_bursts = 0;        // bursts detected
            int stat_decoded = 0;       // header decoded successfully
            int stat_crc_ok = 0;        // CRC passed
            int stat_bit_errors = 0;    // total bit errors (when --payload given)
            int stat_total_bits = 0;    // total bits compared
            bool stream_payload_failure = false; // any payload byte mismatch

            while (true) {
                // Read more data
                if (!reader.eof()) {
                    reader.read_chunk();
                }

                const std::size_t avail = reader.available();
                // Wait until we have enough for burst detection + one packet
                if (avail - search_offset < min_accumulate && !reader.eof()) {
                    continue;
                }

                // Burst detection: use smallest SF (finest resolution).
                auto& detect_ctx = sf_bank.front();
                const auto burst_det = host_sim::detect_burst_ex(
                    reader.data(), avail, detect_ctx.sps, 6.0f,
                    search_offset, tracked_noise_floor, 2);

                if (!burst_det) {
                    if (reader.eof()) {
                        if (packet_index == 0 &&
                            avail - search_offset >=
                            static_cast<std::size_t>(detect_ctx.sps) * 12) {
                            // Fall through with synthetic burst detection
                        } else {
                            break;
                        }
                    } else {
                        if (avail > min_accumulate) {
                            const std::size_t trim =
                                avail - min_accumulate / 2;
                            reader.consume(trim);
                            search_offset = (search_offset > trim)
                                                ? search_offset - trim : 0;
                        }
                        continue;
                    }
                }

                // Burst boundaries (using smallest SF's window size)
                const std::size_t burst_start_raw = burst_det
                    ? burst_det->burst_start : search_offset;
                const float noise_floor = burst_det
                    ? burst_det->noise_floor : 0.0f;
                const std::size_t det_win =
                    static_cast<std::size_t>(detect_ctx.sps);
                const std::size_t bp_win = burst_start_raw / det_win;
                const std::size_t n_total_win = avail / det_win;
                const float thr = noise_floor * 6.0f;

                constexpr std::size_t tail_windows = 5;
                std::size_t quiet_run = 0;
                std::size_t burst_end_win = n_total_win;
                double burst_power_acc = 0.0;
                std::size_t burst_power_count = 0;
                for (std::size_t w = bp_win; w < n_total_win; ++w) {
                    double acc = 0.0;
                    const auto* p = reader.data() + w * det_win;
                    for (std::size_t i = 0; i < det_win; ++i) {
                        acc += static_cast<double>(p[i].real()) * p[i].real() +
                               static_cast<double>(p[i].imag()) * p[i].imag();
                    }
                    const float wp = static_cast<float>(
                        acc / static_cast<double>(det_win));
                    if (wp > thr) {
                        burst_power_acc += wp;
                        ++burst_power_count;
                    }
                    if (wp <= thr) {
                        ++quiet_run;
                        if (quiet_run >= tail_windows) {
                            burst_end_win = w - tail_windows + 1;
                            break;
                        }
                    } else {
                        quiet_run = 0;
                    }
                }

                if (burst_end_win == n_total_win && !reader.eof()) {
                    continue;
                }

                constexpr std::size_t burst_tail_margin = 4;
                const std::size_t burst_end = std::min(
                    (burst_end_win + burst_tail_margin) * det_win, avail);
                const std::size_t burst_len =
                    burst_end > burst_start_raw
                        ? burst_end - burst_start_raw : 0;
                if (burst_len < det_win * 12) {
                    if (options.verbose) {
                        std::cerr << "[stream] short burst (" << burst_len
                                  << " samples), skipping\n";
                    }
                    search_offset = burst_end;
                    continue;
                }
                const std::size_t burst_start = burst_start_raw;

                const float mean_burst_power = (burst_power_count > 0)
                    ? static_cast<float>(burst_power_acc /
                      static_cast<double>(burst_power_count))
                    : (burst_det ? burst_det->signal_power : 0.0f);
                const float snr_linear = (noise_floor > 0.0f)
                    ? (mean_burst_power - noise_floor) / noise_floor
                    : 0.0f;
                const float snr_db = (snr_linear > 0.0f)
                    ? 10.0f * std::log10(snr_linear)
                    : -99.0f;

                const std::span<const std::complex<float>> burst_samples(
                    reader.data() + burst_start, burst_len);

                // ── Multi-SF: select correct SF by demodulating preamble
                //    at fixed offset (burst start).  The correct SF gives
                //    consistent bins; wrong SFs scatter. ──
                SfCtx* best_ctx = &detect_ctx;
                if (sf_bank.size() > 1) {
                    int best_consistency = -1;
                    for (auto& ctx : sf_bank) {
                        if (burst_len <
                            static_cast<std::size_t>(ctx.sps) * 8)
                            continue;
                        ctx.demod->set_frequency_offsets(0.0f, 0, 0.0f);
                        ctx.demod->reset_symbol_counter();
                        const int n = std::min(
                            8, static_cast<int>(burst_len / ctx.sps));
                        // Count most-common bin among first n symbols
                        int bin_counts[4096]{};
                        int n_bins = 1 << ctx.demod->sf();
                        for (int s = 0; s < n; ++s) {
                            int b = ctx.demod->demodulate(
                                burst_samples.data() +
                                s * static_cast<std::size_t>(ctx.sps));
                            if (b >= 0 && b < n_bins) ++bin_counts[b];
                        }
                        int mode_count = 0;
                        for (int i = 0; i < n_bins; ++i) {
                            if (bin_counts[i] > mode_count)
                                mode_count = bin_counts[i];
                        }
                        if (options.verbose) {
                            std::cerr << "[multi-sf-probe] SF="
                                      << ctx.demod->sf()
                                      << " consistency=" << mode_count
                                      << "/" << n << "\n";
                        }
                        if (mode_count > best_consistency) {
                            best_consistency = mode_count;
                            best_ctx = &ctx;
                        }
                    }
                    if (options.verbose) {
                        std::cerr << "[multi-sf] selected SF="
                                  << best_ctx->demod->sf() << "\n";
                    }
                }

                auto& demod = *best_ctx->demod;
                const int sps = best_ctx->sps;
                const int os = best_ctx->os;
                auto metadata = base_meta;
                metadata.sf = demod.sf();

                ++stat_bursts;
                std::cout << "\n=== Packet #" << packet_index << " ===\n";
                {
                    const double t_ms = static_cast<double>(burst_start) / metadata.sample_rate * 1000.0;
                    const double len_ms = static_cast<double>(burst_len) / metadata.sample_rate * 1000.0;
                    std::cout << "Burst at sample " << burst_start
                              << " (" << std::fixed << std::setprecision(1) << t_ms
                              << " ms), " << burst_len << " samples ("
                              << len_ms << " ms), SNR=" << snr_db << " dB\n"
                              << std::defaultfloat << std::setprecision(6);
                }
                std::cout.flush();

                const auto decode_t0 = std::chrono::steady_clock::now();

                // ── Decode this burst (reuse existing pipeline) ──

                // Alignment
                std::size_t alignment_offset = 0;
                int detected_preamble_bin = 0;
                {
                    auto pr = host_sim::find_symbol_alignment_cfo_aware(
                        burst_samples, demod, metadata.preamble_len);
                    alignment_offset = pr.alignment_offset;
                    detected_preamble_bin = pr.preamble_bin;
                }

                // CFO estimation
                float estimated_sfo = 0.0f;
                {
                    const int avail_pream_sym = static_cast<int>(std::min<std::size_t>(
                        (burst_samples.size() > alignment_offset
                             ? (burst_samples.size() - alignment_offset) / static_cast<std::size_t>(sps)
                             : 0),
                        static_cast<std::size_t>(INT_MAX)));
                    const int pream_to_use = std::min(std::max(metadata.preamble_len - 1, 0), avail_pream_sym);
                    if (pream_to_use > 0) {
                        auto freq_est = demod.estimate_frequency_offsets(
                            burst_samples.data() + alignment_offset, pream_to_use);
                        if (detected_preamble_bin != 0) {
                            const int n_bins = 1 << metadata.sf;
                            int signed_bin = detected_preamble_bin;
                            if (signed_bin > n_bins / 2) signed_bin -= n_bins;
                            if (std::abs(signed_bin) > std::abs(freq_est.cfo_int) + 2) {
                                freq_est.cfo_int = signed_bin;
                            }
                        }
                        demod.set_frequency_offsets(freq_est.cfo_frac, freq_est.cfo_int, freq_est.sfo_slope);
                        estimated_sfo = freq_est.sfo_slope;

                        // Report CFO in Hz
                        {
                            const int n_bins = 1 << metadata.sf;
                            const double cfo_bins = static_cast<double>(freq_est.cfo_int) + freq_est.cfo_frac;
                            const double cfo_hz = cfo_bins * static_cast<double>(metadata.bw) / n_bins;
                            std::cout << "CFO=" << std::fixed << std::setprecision(1) << cfo_hz
                                      << " Hz (" << std::setprecision(2) << cfo_bins << " bins)";
                            if (std::abs(freq_est.sfo_slope) > 0.001f) {
                                std::cout << ", SFO=" << std::setprecision(3) << freq_est.sfo_slope << " bins/sym";
                            }
                            std::cout << "\n" << std::defaultfloat << std::setprecision(6);
                        }

                        // Sub-sample alignment refinement (±3 samples).
                        // At low OS, even 1–2 sample error shifts the
                        // dechirped FFT peak and flips marginal symbols.
                        if (os <= 4) {
                            int best_off = 0;
                            int best_c0 = -1;
                            for (int try_off = -3; try_off <= 3; ++try_off) {
                                const auto try_a = static_cast<std::size_t>(
                                    static_cast<std::ptrdiff_t>(alignment_offset) + try_off);
                                if (try_a + 8ULL * sps > burst_samples.size()) continue;
                                demod.set_frequency_offsets(freq_est.cfo_frac,
                                                            freq_est.cfo_int,
                                                            freq_est.sfo_slope);
                                demod.reset_symbol_counter();
                                int c0 = 0;
                                for (int p = 0; p < std::min(pream_to_use, 8); ++p) {
                                    uint16_t v = demod.demodulate(
                                        &burst_samples[try_a +
                                                       static_cast<std::size_t>(p) * sps]);
                                    if (v == 0) ++c0;
                                }
                                if (c0 > best_c0) { best_c0 = c0; best_off = try_off; }
                            }
                            if (best_off != 0) {
                                alignment_offset = static_cast<std::size_t>(
                                    static_cast<std::ptrdiff_t>(alignment_offset) + best_off);
                            }
                        }
                    }
                }

                // Demodulate symbols — no SFO phase correction on preamble grid
                demod.set_frequency_offsets(demod.current_cfo_frac(), demod.current_cfo_int(), 0.0f);
                demod.reset_symbol_counter();
                // Per-symbol CFO tracking (EMA).
                // --cfo-track [alpha] CLI flag or HOST_SIM_CFO_TRACK_ALPHA env.
                {
                    float alpha = options.cfo_track_alpha;
                    if (alpha == 0.0f) {
                        static const char* alpha_env = std::getenv("HOST_SIM_CFO_TRACK_ALPHA");
                        if (alpha_env) alpha = std::stof(alpha_env);
                    }
                    if (alpha > 0.0f) {
                        demod.set_cfo_tracking(alpha, 8);
                    }
                }
                const std::size_t max_sym =
                    (burst_samples.size() - alignment_offset) /
                    static_cast<std::size_t>(sps);
                std::vector<uint16_t> symbols;
                std::vector<host_sim::SymbolLLR> symbol_llrs;
                symbols.reserve(max_sym);
                if (options.soft) symbol_llrs.reserve(max_sym);
                for (std::size_t i = 0; i < max_sym; ++i) {
                    symbols.push_back(demod.demodulate(
                        &burst_samples[alignment_offset +
                                       i * static_cast<std::size_t>(sps)]));
                    if (options.soft) {
                        const auto& mags = demod.get_fft_magnitudes_sq();
                        symbol_llrs.push_back(host_sim::compute_symbol_llrs(
                            mags.data(), metadata.sf,
                            (static_cast<int>(i) < 8) || metadata.ldro,
                            demod.current_cfo_int()));
                    }
                }

                // Try header decode (skip grid scan at high OS)
                HeaderDecodeResult header;
                const bool skip_grid = (os > 4) || (os == 4);

                if (!skip_grid && !header.success) {
                    for (std::size_t c = 0; c + 8 <= symbols.size(); ++c) {
                        auto h = try_decode_header(symbols, c, metadata);
                        if (!h.success) continue;
                        int hlen = h.payload_len > 0 ? h.payload_len : metadata.payload_len;
                        int hcr = h.cr > 0 ? h.cr : metadata.cr;
                        if (metadata.payload_len > 0 && hlen != metadata.payload_len) continue;
                        if (metadata.cr > 0 && hcr != metadata.cr) continue;
                        if (metadata.has_crc && !h.has_crc) continue;
                        header = std::move(h);
                        break;
                    }
                }

                // SFD re-demod fallback
                if (!header.success) {
                    auto sync_pos = host_sim::find_header_symbol_index(
                        symbols, 0x12, metadata.sf);
                    if (!sync_pos)
                        sync_pos = host_sim::find_header_symbol_index(
                            symbols, 0x34, metadata.sf);
                    // Implicit-header fallback: if sync word not found,
                    // estimate data start from known preamble length.
                    // Data starts at: preamble + 2 sync + 2.25 SFD ≈ preamble + 4
                    // (same formula find_header_symbol_index returns: sync_high_pos + 4)
                    if (!sync_pos && metadata.implicit_header) {
                        sync_pos = static_cast<std::size_t>(
                            metadata.preamble_len + 4);
                    }

                    const float saved_cfo_frac = demod.current_cfo_frac();
                    const int saved_cfo_int = demod.current_cfo_int();
                    const int N_bins = 1 << metadata.sf;
                    const double redemod_stride =
                        (estimated_sfo != 0.0f)
                            ? static_cast<double>(sps) *
                                  (1.0 - static_cast<double>(estimated_sfo) / N_bins)
                            : static_cast<double>(sps);

                    if (sync_pos) {
                        const std::size_t quarter =
                            static_cast<std::size_t>(sps / 4);
                        for (int qoff : {1, 0, 2, 3}) {
                            if (header.success) break;
                            const std::size_t data_sample =
                                alignment_offset +
                                *sync_pos * static_cast<std::size_t>(sps) +
                                static_cast<std::size_t>(qoff) * quarter;
                            if (data_sample + 8ULL * sps > burst_samples.size())
                                continue;

                            demod.set_frequency_offsets(
                                saved_cfo_frac, saved_cfo_int, 0.0f);
                            demod.reset_symbol_counter();
                            std::vector<uint16_t> redemod;
                            std::vector<host_sim::SymbolLLR> redemod_llrs;
                            const std::size_t rmax =
                                (burst_samples.size() - data_sample) / sps;
                            // Per-symbol SFO tracking: refine stride using
                            // residual drift from parabolic interpolation.
                            double sfo_accum = 0.0;
                            double sfo_stride = redemod_stride;
                            float sfo_prev_res = 0.0f;
                            float sfo_drift = 0.0f;
                            constexpr float sfo_alpha = 0.01f;
                            constexpr int sfo_delay = 8;
                            for (std::size_t i = 0;
                                 i < std::min<std::size_t>(rmax, 1024); ++i) {
                                const std::size_t off =
                                    static_cast<std::size_t>(
                                        std::round(sfo_accum));
                                if (data_sample + off +
                                        static_cast<std::size_t>(sps) >
                                    burst_samples.size())
                                    break;
                                redemod.push_back(demod.demodulate(
                                    &burst_samples[data_sample + off]));
                                if (options.soft) {
                                    const auto& mags = demod.get_fft_magnitudes_sq();
                                    redemod_llrs.push_back(host_sim::compute_symbol_llrs(
                                        mags.data(), metadata.sf,
                                        (static_cast<int>(i) < 8) || metadata.ldro,
                                        demod.current_cfo_int()));
                                }
                                // Adaptive stride: track residual slope.
                                if (static_cast<int>(i) >= sfo_delay) {
                                    float dr = demod.last_residual() - sfo_prev_res;
                                    if (dr > 0.5f) dr -= 1.0f;
                                    if (dr < -0.5f) dr += 1.0f;
                                    sfo_drift += sfo_alpha * (dr - sfo_drift);
                                    sfo_stride = redemod_stride -
                                        static_cast<double>(sps) *
                                        static_cast<double>(sfo_drift) / N_bins;
                                }
                                sfo_prev_res = demod.last_residual();
                                sfo_accum += sfo_stride;
                            }

                            if (metadata.implicit_header) {
                                // Build implicit header matching batch path:
                                // deinterleave first block at CR=4 (header rate),
                                // prepend 5 zero nibbles (placeholder for absent
                                // explicit header fields).
                                HeaderDecodeResult imp;
                                const std::size_t hdr_syms_cnt = std::min<std::size_t>(8, redemod.size());
                                std::vector<uint16_t> first_block(
                                    redemod.begin(),
                                    redemod.begin() + static_cast<std::ptrdiff_t>(hdr_syms_cnt));
                                host_sim::DeinterleaverConfig hdr_cfg{
                                    metadata.sf, 4, true, metadata.ldro};
                                std::size_t consumed_block = 0;
                                auto codewords = host_sim::deinterleave(
                                    first_block, hdr_cfg, consumed_block);
                                auto nibs = host_sim::hamming_decode_block(
                                    codewords, true, 4);
                                imp.success = true;
                                imp.payload_len = metadata.payload_len;
                                imp.cr = metadata.cr;
                                imp.has_crc = metadata.has_crc;
                                imp.checksum_field = -1;
                                imp.checksum_computed = -1;
                                imp.consumed_symbols = consumed_block > 0
                                    ? static_cast<int>(consumed_block) : 8;
                                imp.codewords.assign(codewords.begin(), codewords.end());
                                imp.nibbles.assign(5, 0);
                                imp.nibbles.insert(imp.nibbles.end(),
                                                   nibs.begin(), nibs.end());

                                // CRC-guided timing sweep for implicit header
                                if (metadata.has_crc &&
                                    !probe_payload_crc(redemod, imp, metadata)) {
                                    const int max_adj = std::max(os, 4) + 2;
                                    bool found_adj = false;
                                    for (int adj = -1; std::abs(adj) <= max_adj;
                                         adj = adj > 0 ? -adj - 1 : -adj) {
                                        const auto adj_data =
                                            static_cast<std::size_t>(
                                                static_cast<std::ptrdiff_t>(
                                                    data_sample) + adj);
                                        if (adj_data + 8ULL * sps >
                                            burst_samples.size())
                                            continue;
                                        demod.set_frequency_offsets(
                                            saved_cfo_frac, saved_cfo_int, 0.0f);
                                        demod.reset_symbol_counter();
                                        std::vector<uint16_t> adj_syms;
                                        const std::size_t adj_max =
                                            (burst_samples.size() - adj_data) / sps;
                                        for (std::size_t i = 0;
                                             i < std::min<std::size_t>(adj_max, 200);
                                             ++i) {
                                            const std::size_t so =
                                                static_cast<std::size_t>(
                                                    std::round(i * redemod_stride));
                                            if (adj_data + so +
                                                    static_cast<std::size_t>(sps) >
                                                burst_samples.size())
                                                break;
                                            adj_syms.push_back(demod.demodulate(
                                                &burst_samples[adj_data + so]));
                                        }
                                        // Rebuild implicit header for adjusted symbols
                                        std::size_t adj_consumed = 0;
                                        std::vector<uint16_t> adj_first(
                                            adj_syms.begin(),
                                            adj_syms.begin() + std::min<std::ptrdiff_t>(
                                                8, static_cast<std::ptrdiff_t>(adj_syms.size())));
                                        auto adj_cw = host_sim::deinterleave(
                                            adj_first, hdr_cfg, adj_consumed);
                                        auto adj_nibs = host_sim::hamming_decode_block(
                                            adj_cw, true, 4);
                                        HeaderDecodeResult adj_imp;
                                        adj_imp.success = true;
                                        adj_imp.payload_len = metadata.payload_len;
                                        adj_imp.cr = metadata.cr;
                                        adj_imp.has_crc = metadata.has_crc;
                                        adj_imp.consumed_symbols = adj_consumed > 0
                                            ? static_cast<int>(adj_consumed) : 8;
                                        adj_imp.nibbles.assign(5, 0);
                                        adj_imp.nibbles.insert(adj_imp.nibbles.end(),
                                                               adj_nibs.begin(),
                                                               adj_nibs.end());
                                        if (probe_payload_crc(
                                                adj_syms, adj_imp, metadata)) {
                                            redemod = std::move(adj_syms);
                                            redemod_llrs.clear();
                                            imp = std::move(adj_imp);
                                            std::cout << "SFD re-demod: implicit data start "
                                                         "refined by "
                                                      << adj
                                                      << " samples (CRC verified)\n";
                                            found_adj = true;
                                            break;
                                        }
                                    }
                                    if (!found_adj) continue;  // Try next qoff
                                }
                                header = std::move(imp);
                                symbols = std::move(redemod);
                                symbol_llrs = std::move(redemod_llrs);
                                break;
                            }

                            auto hdr = try_decode_header(redemod, 0, metadata);
                            if (!hdr.success) continue;
                            int hlen = hdr.payload_len > 0
                                           ? hdr.payload_len
                                           : metadata.payload_len;
                            int hcr =
                                hdr.cr > 0 ? hdr.cr : metadata.cr;
                            if (metadata.payload_len > 0 &&
                                hlen != metadata.payload_len)
                                continue;
                            if (metadata.cr > 0 && hcr != metadata.cr)
                                continue;
                            if (metadata.has_crc && !hdr.has_crc)
                                continue;

                            // CRC-guided timing sweep
                            if ((hdr.has_crc || metadata.has_crc) &&
                                !probe_payload_crc(redemod, hdr, metadata)) {
                                const int max_adj = std::max(os, 4) + 2;
                                for (int adj = -1; std::abs(adj) <= max_adj;
                                     adj = adj > 0 ? -adj - 1 : -adj) {
                                    const auto adj_data =
                                        static_cast<std::size_t>(
                                            static_cast<std::ptrdiff_t>(
                                                data_sample) +
                                            adj);
                                    if (adj_data + 8ULL * sps >
                                        burst_samples.size())
                                        continue;
                                    demod.set_frequency_offsets(
                                        saved_cfo_frac, saved_cfo_int, 0.0f);
                                    demod.reset_symbol_counter();
                                    std::vector<uint16_t> adj_syms;
                                    const std::size_t adj_max =
                                        (burst_samples.size() - adj_data) / sps;
                                    for (std::size_t i = 0;
                                         i < std::min<std::size_t>(adj_max, 200);
                                         ++i) {
                                        const std::size_t so =
                                            static_cast<std::size_t>(
                                                std::round(i * redemod_stride));
                                        if (adj_data + so +
                                                static_cast<std::size_t>(sps) >
                                            burst_samples.size())
                                            break;
                                        adj_syms.push_back(demod.demodulate(
                                            &burst_samples[adj_data + so]));
                                    }
                                    auto adj_hdr = try_decode_header(
                                        adj_syms, 0, metadata);
                                    if (!adj_hdr.success) continue;
                                    if (probe_payload_crc(
                                            adj_syms, adj_hdr, metadata)) {
                                        redemod = std::move(adj_syms);
                                        redemod_llrs.clear();
                                        hdr = std::move(adj_hdr);
                                        break;
                                    }
                                }
                            }

                            std::cout << "SFD re-demod: header found with "
                                         "quarter offset "
                                      << qoff << " (sync at symbol "
                                      << (*sync_pos - 4) << ")\n";
                            header = std::move(hdr);
                            symbols = std::move(redemod);
                            symbol_llrs = std::move(redemod_llrs);
                            break;
                        }
                    }

                    // ── OS=2 upsample fallback (streaming) ──────────
                    // When native-OS decode at OS=1 fails or produces
                    // CRC-invalid payload, upsample burst by 2x and
                    // retry with SFO rate compensation sweep.
                    bool need_os2 = !header.success;
                    if (!need_os2 && os == 1 && sync_pos &&
                        (header.has_crc || metadata.has_crc) &&
                        !probe_payload_crc(symbols, header, metadata)) {
                        need_os2 = true;
                    }
                    if (need_os2 && sync_pos && os == 1) {
                        auto fallback_header = header;
                        auto fallback_symbols = symbols;
                        auto fallback_llrs = symbol_llrs;
                        header.success = false;

                        const std::size_t burst_start = alignment_offset;
                        const std::size_t burst_len = burst_samples.size() - burst_start;
                        auto up = upsample_2x(&burst_samples[burst_start], burst_len);

                        const int sps_os2 = sps * 2;
                        const std::size_t quarter_os2 =
                            static_cast<std::size_t>(sps_os2 / 4);
                        host_sim::FftDemodulator demod_os2(metadata.sf,
                                                metadata.bw * 2,
                                                metadata.bw);

                        std::vector<std::pair<int,int>> hdr_hit_pairs;
                        for (int os2_pass = 0;
                             os2_pass < 2 && !header.success; ++os2_pass) {
                        for (int sfo_cand = 0; std::abs(sfo_cand) <= 100;
                             sfo_cand = sfo_cand >= 0 ? -sfo_cand - 10
                                                      : -sfo_cand) {
                            if (header.success) break;
                            const double stride =
                                static_cast<double>(sps_os2) *
                                (1.0 - static_cast<double>(sfo_cand) * 1e-6);

                        for (int qoff : {1, 0, 2, 3}) {
                            if (header.success) break;
                            if (os2_pass == 0 && qoff != 1) continue;
                            if (os2_pass == 1) {
                                bool was_hit = false;
                                for (auto& p : hdr_hit_pairs)
                                    if (p.first == sfo_cand && p.second == qoff)
                                        was_hit = true;
                                if (!was_hit) continue;
                            }
                            const std::size_t data_sample_os2 =
                                *sync_pos * static_cast<std::size_t>(sps_os2) +
                                static_cast<std::size_t>(qoff) * quarter_os2;
                            if (data_sample_os2 + 8ULL * sps_os2 > up.size())
                                continue;

                            demod_os2.set_frequency_offsets(saved_cfo_frac,
                                                           saved_cfo_int,
                                                           0.0f);
                            demod_os2.reset_symbol_counter();
                            std::vector<uint16_t> os2_syms;
                            std::vector<host_sim::SymbolLLR> os2_llrs;

                            // Demod first 8 symbols (header probe)
                            for (std::size_t i = 0; i < 8; ++i) {
                                const auto pos = static_cast<std::size_t>(
                                    std::round(static_cast<double>(
                                                   data_sample_os2) +
                                               static_cast<double>(i) * stride));
                                if (pos + sps_os2 > up.size()) break;
                                os2_syms.push_back(demod_os2.demodulate(&up[pos]));
                                if (options.soft) {
                                    const auto& mags = demod_os2.get_fft_magnitudes_sq();
                                    os2_llrs.push_back(host_sim::compute_symbol_llrs(
                                        mags.data(), metadata.sf,
                                        true, demod_os2.current_cfo_int()));
                                }
                            }
                            if (os2_syms.size() < 8) continue;

                            // Header validation
                            HeaderDecodeResult os2_hdr;
                            int hlen = 0, hcr = 0;
                            if (metadata.implicit_header) {
                                host_sim::DeinterleaverConfig hdr_cfg{
                                    metadata.sf, 4, true, metadata.ldro};
                                std::size_t consumed_block = 0;
                                std::vector<uint16_t> first8(os2_syms.begin(),
                                    os2_syms.begin() + 8);
                                auto cw = host_sim::deinterleave(
                                    first8, hdr_cfg, consumed_block);
                                auto nibs = host_sim::hamming_decode_block(cw, true, 4);
                                os2_hdr.success = true;
                                os2_hdr.payload_len = metadata.payload_len;
                                os2_hdr.cr = metadata.cr;
                                os2_hdr.has_crc = metadata.has_crc;
                                os2_hdr.consumed_symbols = consumed_block > 0
                                    ? static_cast<int>(consumed_block) : 8;
                                os2_hdr.nibbles.assign(5, 0);
                                os2_hdr.nibbles.insert(os2_hdr.nibbles.end(),
                                                       nibs.begin(), nibs.end());
                                hlen = metadata.payload_len;
                                hcr = metadata.cr;
                            } else {
                                os2_hdr = try_decode_header(os2_syms, 0, metadata);
                                if (!os2_hdr.success) continue;
                                hlen = os2_hdr.payload_len > 0
                                           ? os2_hdr.payload_len
                                           : metadata.payload_len;
                                hcr = os2_hdr.cr > 0 ? os2_hdr.cr : metadata.cr;
                                if (metadata.payload_len > 0 &&
                                    hlen != metadata.payload_len) continue;
                                if (metadata.cr > 0 && hcr != metadata.cr) continue;
                                if (metadata.has_crc && !os2_hdr.has_crc) continue;
                            }

                            if (os2_pass == 0) {
                                hdr_hit_pairs.emplace_back(sfo_cand, qoff);
                                continue;
                            }

                            // Pass 1: full payload demod + CRC check
                            const int payload_cw = hcr + 4;
                            const std::size_t nibbles_needed =
                                static_cast<std::size_t>(hlen) * 2 +
                                (os2_hdr.has_crc || metadata.has_crc ? 4 : 0);
                            const std::size_t header_nibs =
                                os2_hdr.nibbles.size() > 5
                                    ? os2_hdr.nibbles.size() - 5 : 0;
                            const std::size_t data_nibs_needed =
                                nibbles_needed > header_nibs
                                    ? nibbles_needed - header_nibs : 0;
                            const std::size_t data_blocks =
                                (data_nibs_needed + payload_cw - 1) /
                                static_cast<std::size_t>(payload_cw);
                            const std::size_t max_data_syms =
                                data_blocks * static_cast<std::size_t>(payload_cw) + 4;
                            const std::size_t total_syms =
                                8 + max_data_syms;

                            // Demod remaining symbols at variable stride
                            for (std::size_t i = 8; i < total_syms; ++i) {
                                const auto pos = static_cast<std::size_t>(
                                    std::round(static_cast<double>(
                                                   data_sample_os2) +
                                               static_cast<double>(i) * stride));
                                if (pos + sps_os2 > up.size()) break;
                                os2_syms.push_back(demod_os2.demodulate(&up[pos]));
                                if (options.soft) {
                                    const auto& mags = demod_os2.get_fft_magnitudes_sq();
                                    os2_llrs.push_back(host_sim::compute_symbol_llrs(
                                        mags.data(), metadata.sf,
                                        metadata.ldro,
                                        demod_os2.current_cfo_int()));
                                }
                            }

                            // CRC probe + timing adjustment sweep
                            if ((os2_hdr.has_crc || metadata.has_crc) &&
                                probe_payload_crc(os2_syms, os2_hdr, metadata)) {
                                std::cout << "OS=2 fallback: CRC OK (sfo="
                                          << sfo_cand << " ppm, qoff="
                                          << qoff << ")\n";
                                header = std::move(os2_hdr);
                                symbols = std::move(os2_syms);
                                symbol_llrs = std::move(os2_llrs);
                                break;
                            }
                            // Timing adjustment sweep (±3 samples)
                            for (int adj = -1; std::abs(adj) <= 3;
                                 adj = adj > 0 ? -adj - 1 : -adj) {
                                const auto adj_data = static_cast<std::size_t>(
                                    static_cast<std::ptrdiff_t>(data_sample_os2) + adj);
                                if (adj_data + 8ULL * sps_os2 > up.size()) continue;
                                demod_os2.set_frequency_offsets(saved_cfo_frac,
                                                               saved_cfo_int, 0.0f);
                                demod_os2.reset_symbol_counter();
                                std::vector<uint16_t> adj_syms;
                                for (std::size_t i = 0; i < total_syms; ++i) {
                                    const auto pos = static_cast<std::size_t>(
                                        std::round(static_cast<double>(adj_data) +
                                                   static_cast<double>(i) * stride));
                                    if (pos + sps_os2 > up.size()) break;
                                    adj_syms.push_back(demod_os2.demodulate(&up[pos]));
                                }
                                HeaderDecodeResult adj_hdr;
                                if (metadata.implicit_header) {
                                    host_sim::DeinterleaverConfig hdr_cfg{
                                        metadata.sf, 4, true, metadata.ldro};
                                    std::size_t adj_consumed = 0;
                                    std::vector<uint16_t> adj_first8(adj_syms.begin(),
                                        adj_syms.begin() + std::min<std::ptrdiff_t>(
                                            8, static_cast<std::ptrdiff_t>(adj_syms.size())));
                                    auto adj_cw = host_sim::deinterleave(
                                        adj_first8, hdr_cfg, adj_consumed);
                                    auto adj_nibs = host_sim::hamming_decode_block(
                                        adj_cw, true, 4);
                                    adj_hdr.success = true;
                                    adj_hdr.payload_len = metadata.payload_len;
                                    adj_hdr.cr = metadata.cr;
                                    adj_hdr.has_crc = metadata.has_crc;
                                    adj_hdr.consumed_symbols = adj_consumed > 0
                                        ? static_cast<int>(adj_consumed) : 8;
                                    adj_hdr.nibbles.assign(5, 0);
                                    adj_hdr.nibbles.insert(adj_hdr.nibbles.end(),
                                                           adj_nibs.begin(),
                                                           adj_nibs.end());
                                } else {
                                    adj_hdr = try_decode_header(adj_syms, 0, metadata);
                                    if (!adj_hdr.success) continue;
                                }
                                if (probe_payload_crc(adj_syms, adj_hdr, metadata)) {
                                    std::cout << "OS=2 fallback: CRC OK after adj="
                                              << adj << " (sfo=" << sfo_cand
                                              << " ppm, qoff=" << qoff << ")\n";
                                    header = std::move(adj_hdr);
                                    symbols = std::move(adj_syms);
                                    symbol_llrs.clear();
                                    break;
                                }
                            }
                        }}}  // qoff / sfo_cand / os2_pass

                        // If OS=2 failed, restore native decode.
                        if (!header.success) {
                            header = std::move(fallback_header);
                            symbols = std::move(fallback_symbols);
                            symbol_llrs = std::move(fallback_llrs);
                        }
                    }
                }

                // Payload decode
                if (header.success) {
                    ++stat_decoded;
                    const int payload_len = header.payload_len > 0
                                          ? header.payload_len
                                          : metadata.payload_len;
                    const int active_cr = header.cr > 0 ? header.cr : metadata.cr;
                    const bool has_crc = header.has_crc || metadata.has_crc;

                    std::cout << "Header: len=" << payload_len
                              << " cr=" << active_cr
                              << " crc=" << (has_crc ? "yes" : "no") << "\n";

                    // Payload nibble target
                    std::size_t nibble_target = static_cast<std::size_t>(payload_len) * 2;
                    if (has_crc) nibble_target += 4;

                    // At SF >= 8 the header block produces SF-2 nibbles:
                    // first 5 are header fields, rest spill into payload.
                    std::vector<uint8_t> payload_nibbles;
                    if (header.nibbles.size() > 5) {
                        for (std::size_t i = 5; i < header.nibbles.size(); ++i) {
                            payload_nibbles.push_back(header.nibbles[i]);
                        }
                    }

                    // Decode data symbols block-by-block
                    const int payload_cw_len = active_cr + 4;
                    std::size_t sym_cursor = header.consumed_symbols;
                    host_sim::DeinterleaverConfig payload_cfg{
                        metadata.sf, active_cr, false, metadata.ldro};

                    while (static_cast<std::ptrdiff_t>(sym_cursor) + payload_cw_len <=
                               static_cast<std::ptrdiff_t>(symbols.size()) &&
                           payload_nibbles.size() < nibble_target) {
                        std::vector<uint16_t> block(
                            symbols.begin() + static_cast<std::ptrdiff_t>(sym_cursor),
                            symbols.begin() + static_cast<std::ptrdiff_t>(sym_cursor) + payload_cw_len);
                        std::size_t consumed = 0;

                        std::vector<uint8_t> nibs;
                        if (options.soft &&
                            sym_cursor + static_cast<std::size_t>(payload_cw_len) <= symbol_llrs.size()) {
                            std::vector<host_sim::SymbolLLR> block_llrs(
                                symbol_llrs.begin() + static_cast<std::ptrdiff_t>(sym_cursor),
                                symbol_llrs.begin() + static_cast<std::ptrdiff_t>(sym_cursor) + payload_cw_len);
                            nibs = host_sim::soft_decode_block(
                                block_llrs, metadata.sf, active_cr,
                                false, metadata.ldro, consumed);
                        } else {
                            auto codewords = host_sim::deinterleave(block, payload_cfg, consumed);
                            nibs = host_sim::hamming_decode_block(codewords, false, active_cr);
                        }
                        if (consumed == 0) break;
                        sym_cursor += consumed;
                        payload_nibbles.insert(payload_nibbles.end(), nibs.begin(), nibs.end());
                    }

                    // Dewhiten at nibble level, pack into bytes
                    // (matches GNU Radio gr-lora_sdr dewhitening convention)
                    host_sim::WhiteningSequencer seq;
                    auto whitening = seq.sequence(payload_nibbles.size() / 2);

                    std::vector<uint8_t> dewhitened;
                    dewhitened.reserve(payload_nibbles.size() / 2);
                    for (std::size_t i = 0; i + 1 < payload_nibbles.size(); i += 2) {
                        const std::size_t byte_idx = i / 2;
                        uint8_t low_nib, high_nib;
                        if (byte_idx < static_cast<std::size_t>(payload_len)) {
                            const uint8_t w = (byte_idx < whitening.size()) ? whitening[byte_idx] : 0;
                            low_nib = (payload_nibbles[i] & 0xF) ^ (w & 0x0F);
                            high_nib = (payload_nibbles[i + 1] & 0xF) ^ ((w >> 4) & 0x0F);
                        } else {
                            low_nib = payload_nibbles[i] & 0xF;
                            high_nib = payload_nibbles[i + 1] & 0xF;
                        }
                        dewhitened.push_back(static_cast<uint8_t>((high_nib << 4) | low_nib));
                    }
                    if (dewhitened.size() > static_cast<std::size_t>(payload_len) + (has_crc ? 2u : 0u)) {
                        dewhitened.resize(static_cast<std::size_t>(payload_len) + (has_crc ? 2u : 0u));
                    }

                    // Print payload
                    std::cout << "Payload bytes (dewhitened):";
                    for (std::size_t i = 0;
                         i < std::min<std::size_t>(dewhitened.size(),
                                                    static_cast<std::size_t>(payload_len));
                         ++i) {
                        std::cout << ' ' << std::hex << std::setw(2)
                                  << std::setfill('0')
                                  << static_cast<int>(dewhitened[i]);
                    }
                    std::cout << std::dec << "\n";

                    // ASCII
                    std::cout << "Payload ASCII: ";
                    for (std::size_t i = 0;
                         i < std::min<std::size_t>(dewhitened.size(),
                                                    static_cast<std::size_t>(payload_len));
                         ++i) {
                        const char c = static_cast<char>(dewhitened[i]);
                        std::cout << (std::isprint(static_cast<unsigned char>(c)) ? c : '.');
                    }
                    std::cout << "\n";

                    // CRC check — GNU Radio convention:
                    // 1. CRC-16/CCITT on first payload_len-2 bytes
                    // 2. XOR with last 2 payload bytes
                    if (has_crc && payload_len >= 2 &&
                        static_cast<int>(dewhitened.size()) >= payload_len + 2) {
                        std::vector<uint8_t> crc_data(
                            dewhitened.begin(),
                            dewhitened.begin() + payload_len - 2);
                        uint16_t crc = compute_raw_crc16(crc_data);
                        crc ^= dewhitened[payload_len - 1];
                        crc ^= static_cast<uint16_t>(dewhitened[payload_len - 2]) << 8;
                        const uint16_t decoded_crc =
                            static_cast<uint16_t>(dewhitened[payload_len]) |
                            (static_cast<uint16_t>(dewhitened[payload_len + 1]) << 8);
                        const bool ok = (decoded_crc == crc);
                        if (ok) ++stat_crc_ok;
                        std::cout << "[payload] CRC decoded=0x" << std::hex
                                  << std::setw(4) << std::setfill('0')
                                  << decoded_crc << " computed=0x"
                                  << std::setw(4) << std::setfill('0') << crc
                                  << std::dec << (ok ? " OK" : " MISMATCH")
                                  << "\n";
                        if (!ok && !options.payload.empty()) {
                            stream_payload_failure = true;
                        }
                    }

                    // BER: compare payload against expected (--payload)
                    if (!options.payload.empty()) {
                        const auto& ref = options.payload;
                        const int cmp_len = std::min(
                            static_cast<int>(ref.size()), payload_len);
                        for (int b = 0; b < cmp_len; ++b) {
                            uint8_t diff = dewhitened[b] ^
                                           static_cast<uint8_t>(ref[b]);
                            stat_bit_errors += __builtin_popcount(diff);
                        }
                        stat_total_bits += cmp_len * 8;

                        // Byte-exact payload verification
                        bool match = (static_cast<int>(ref.size()) == payload_len);
                        if (match) {
                            for (int b = 0; b < payload_len; ++b) {
                                if (dewhitened[b] != static_cast<uint8_t>(ref[b])) {
                                    match = false;
                                    break;
                                }
                            }
                        }
                        if (!match) {
                            stream_payload_failure = true;
                            std::cout << "[payload] MISMATCH (stream packet #"
                                      << packet_index << ")\n";
                        }
                    }
                } else {
                    std::cout << "[stream] packet #" << packet_index
                              << ": header decode failed\n";
                }

                const auto decode_t1 = std::chrono::steady_clock::now();
                const auto decode_us = std::chrono::duration_cast<
                    std::chrono::microseconds>(decode_t1 - decode_t0).count();
                std::cout << "[stream] decode latency: " << std::fixed
                          << std::setprecision(1) << decode_us / 1000.0
                          << " ms\n" << std::defaultfloat << std::setprecision(6);
                std::cout.flush();

                // Advance past this burst
                search_offset = burst_end;
                ++packet_index;

                // Update adaptive noise floor estimate for next burst
                if (noise_floor > 0.0f) {
                    tracked_noise_floor = noise_floor;
                }

                // Periodically compact the buffer
                if (search_offset > min_accumulate) {
                    reader.consume(search_offset);
                    search_offset = 0;
                }
            }

            std::cout << "\n[stream] EOF — " << packet_index
                      << " packet(s) processed\n";

            // PER/BER summary
            if (options.per_stats) {
                std::cout << "\n=== PER/BER Statistics ===\n"
                          << "  Bursts detected : " << stat_bursts << "\n"
                          << "  Headers decoded : " << stat_decoded << "\n"
                          << "  CRC OK          : " << stat_crc_ok << "\n";
                if (stat_bursts > 0) {
                    const double per = 1.0 - static_cast<double>(stat_crc_ok) /
                                             static_cast<double>(stat_bursts);
                    std::cout << "  PER             : " << std::fixed
                              << std::setprecision(4) << per << "\n"
                              << std::defaultfloat << std::setprecision(6);
                }
                if (stat_total_bits > 0) {
                    const double ber = static_cast<double>(stat_bit_errors) /
                                       static_cast<double>(stat_total_bits);
                    std::cout << "  BER             : " << std::scientific
                              << std::setprecision(2) << ber
                              << " (" << stat_bit_errors << "/" << stat_total_bits << " bits)\n"
                              << std::defaultfloat << std::setprecision(6);
                }
                std::cout << "==========================\n";
            }

            return stream_payload_failure ? EXIT_FAILURE : EXIT_SUCCESS;
        }

        // ── Batch mode (original path) ──────────────────────────
        std::vector<std::complex<float>> samples;
        if (options.read_stdin) {
            if (options.verbose) std::cerr << "[debug] reading IQ from stdin"
                << (options.iq_format == Options::IqFormat::hackrf ? " (hackrf int8)" : " (cf32)")
                << std::endl;
            if (options.iq_format == Options::IqFormat::hackrf) {
                samples = host_sim::load_hackrf_stdin();
            } else {
                samples = host_sim::load_cf32_stdin();
            }
            if (options.verbose) std::cerr << "[debug] read " << samples.size() << " samples from stdin" << std::endl;
        } else {
            samples = host_sim::load_cf32(options.iq_file);
        }
        if (options.verbose) std::cerr << "[debug] loaded samples=" << samples.size() << std::endl;
        const auto stats = host_sim::analyse_capture(samples);
        if (options.verbose) std::cerr << "[debug] analysed capture" << std::endl;

        SummaryReport summary;
        if (!options.iq_file.empty()) {
            summary.capture_path = options.iq_file.generic_string();
        }
        summary.stats = stats;

        std::vector<uint16_t> symbols;
        std::vector<host_sim::SymbolLLR> symbol_llrs;
        std::size_t alignment_samples = 0;

        std::cout << "Loaded capture: " << (options.read_stdin ? "<stdin>" : options.iq_file.string()) << "\n"
                  << "  Samples: " << stats.sample_count << "\n"
                  << "  Min |x|: " << std::setprecision(6) << stats.min_magnitude << "\n"
                  << "  Max |x|: " << std::setprecision(6) << stats.max_magnitude << "\n"
                  << "  Mean power: " << std::setprecision(6) << stats.mean_power << "\n";

        if (options.verbose) std::cerr << "[debug] preparing metadata" << std::endl;
        std::optional<host_sim::LoRaMetadata> metadata;
        if (options.metadata) {
            metadata = host_sim::load_metadata(*options.metadata);
        } else if (!options.read_stdin) {
            auto guess = options.iq_file;
            guess.replace_extension(".json");
            if (std::filesystem::exists(guess)) {
                metadata = host_sim::load_metadata(guess);
            }
        }
        if (options.verbose) std::cerr << "[debug] metadata loaded? " << (metadata.has_value()) << std::endl;
        if (metadata) {
            summary.metadata = *metadata;
        }

        bool compare_failure = false;
        bool payload_failure = false;  // payload content or CRC mismatch
        std::size_t total_stage_mismatches = 0;
        std::size_t reference_mismatches = 0;

        if (metadata) {
            StageOutputs stage_outputs;
            bool have_stage_outputs = false;
            std::cout << "Metadata: SF=" << metadata->sf << ", CR=" << metadata->cr
                      << ", BW=" << metadata->bw << ", Fs=" << metadata->sample_rate
                      << ", payload_len=" << metadata->payload_len << "\n";
            if (options.verbose) std::cerr << "[debug] after metadata print" << std::endl;

            host_sim::FftDemodulator demod(metadata->sf, metadata->sample_rate, metadata->bw);
            host_sim::FftDemodReference demod_ref(metadata->sf, metadata->sample_rate, metadata->bw);
            if (options.verbose) std::cerr << "[debug] demodulators constructed" << std::endl;
            const int sps = demod.samples_per_symbol();
            int detected_preamble_bin = 0;

            // --- Stage 0: burst detection ---
            // If the capture contains noise before the signal, skip to
            // the burst start so that alignment search sees the preamble.
            std::size_t multi_search_offset = 0;
            int multi_packet_index = 0;
            // Tracks where re-demod data symbols start (alignment +
            // sync_pos + quarter-offset), for multi-packet advance.
            std::size_t data_start_sample = 0;

          for (;;) { // multi-packet loop (runs once unless --multi)
            const auto burst_result = host_sim::detect_burst_start(
                samples, sps, 6.0f, multi_search_offset);
            const std::size_t burst_offset = burst_result.value_or(0);
            if (!burst_result && multi_search_offset > 0) {
                break; // no more bursts in --multi mode
            }
            if (burst_offset > 0) {
                if (options.multi_packet) {
                    std::cout << "\n=== Packet #" << multi_packet_index << " ===\n";
                }
                std::cout << "Burst detected at sample " << burst_offset
                          << " (" << static_cast<double>(burst_offset) /
                                     metadata->sample_rate * 1000.0
                          << " ms)" << std::endl;
            }

            // Create a view of samples starting at the burst.
            const std::complex<float>* burst_samples = samples.data() + burst_offset;
            const std::size_t burst_len = samples.size() - burst_offset;
            // Limit the alignment-search window to preamble + SFD + margin
            // to avoid copying the entire remaining capture.
            const std::size_t align_window = static_cast<std::size_t>(sps) *
                static_cast<std::size_t>(metadata->preamble_len + 6);
            const std::size_t view_len = std::min(burst_len, align_window);
            const std::span<const std::complex<float>> burst_view(
                burst_samples, view_len);

            // --- Stage 1: alignment ---
            // At low oversampling (os<=4), the polyphase anti-aliasing is weak
            // so legacy alignment (bin-0 scoring) is more reliable.  At high
            // oversampling (os>4), the legacy approach fails due to aliasing,
            // so we use the CFO-aware alignment (magnitude scoring).
            const int os = demod.oversample_factor();
            if (os > 4) {
                auto pr = host_sim::find_symbol_alignment_cfo_aware(
                    burst_view, demod, metadata->preamble_len);
                alignment_samples = burst_offset + pr.alignment_offset;
                detected_preamble_bin = pr.preamble_bin;
                std::cout << "Alignment offset: " << alignment_samples << " samples"
                          << " (preamble_bin=" << pr.preamble_bin
                          << ", score=" << pr.score << ")" << std::endl;
            } else {
                // Legacy alignment: fast, works for zero/small CFO.
                alignment_samples = burst_offset +
                    host_sim::find_symbol_alignment(burst_view, demod,
                                                    metadata->preamble_len);
                std::cout << "Alignment offset: " << alignment_samples
                          << " samples" << std::endl;
            }
            if (options.verbose) std::cerr << "[debug] alignment done" << std::endl;

            const int available_symbols = static_cast<int>(std::min<std::size_t>(
                (samples.size() > alignment_samples
                     ? (samples.size() - alignment_samples) / static_cast<std::size_t>(sps)
                     : 0),
                static_cast<std::size_t>(INT_MAX)));
            const int preamble_symbols_to_use =
                std::min(std::max(metadata->preamble_len - 1, 0), available_symbols);
            float estimated_sfo = 0.0f;
            if (preamble_symbols_to_use > 0) {
                auto freq_est = demod.estimate_frequency_offsets(
                    samples.data() + alignment_samples,
                    preamble_symbols_to_use);
                // If the CFO-aware search detected a large preamble bin that
                // estimate_frequency_offsets missed, use the detected value.
                if (detected_preamble_bin != 0) {
                    const int n_bins = 1 << metadata->sf;
                    int signed_bin = detected_preamble_bin;
                    if (signed_bin > n_bins / 2) signed_bin -= n_bins;
                    if (std::abs(signed_bin) > std::abs(freq_est.cfo_int) + 2) {
                        freq_est.cfo_int = signed_bin;
                    }
                }
                demod.set_frequency_offsets(freq_est.cfo_frac,
                                            freq_est.cfo_int,
                                            freq_est.sfo_slope);

                // --- Sub-sample alignment refinement ---
                // The alignment algorithm may be off by a few samples.
                // At low oversampling (os ≤ 4), even a 1–2 sample error
                // shifts the dechirped FFT peak by a fraction of a bin,
                // which can flip the rounded bin on marginal symbols.
                // Try a few offsets around the detected alignment and pick
                // the one whose preamble symbols most agree on bin 0.
                if (os <= 4 && !options.compare_root) {
                    int best_offset = 0;
                    int best_count_0 = -1;
                    for (int try_off = -3; try_off <= 3; ++try_off) {
                        const auto try_align = static_cast<std::size_t>(
                            static_cast<std::ptrdiff_t>(alignment_samples) + try_off);
                        if (try_align + 8ULL * sps > samples.size()) continue;
                        demod.set_frequency_offsets(freq_est.cfo_frac,
                                                    freq_est.cfo_int,
                                                    freq_est.sfo_slope);
                        demod.reset_symbol_counter();
                        int c0 = 0;
                        for (int p = 0; p < std::min(preamble_symbols_to_use, 8); ++p) {
                            uint16_t v = demod.demodulate(
                                &samples[try_align +
                                         static_cast<std::size_t>(p) * sps]);
                            if (v == 0) ++c0;
                        }
                        if (c0 > best_count_0) {
                            best_count_0 = c0;
                            best_offset = try_off;
                        }
                    }
                    if (best_offset != 0) {
                        alignment_samples = static_cast<std::size_t>(
                            static_cast<std::ptrdiff_t>(alignment_samples) +
                            best_offset);
                    }
                }

                // For the main demod loop (preamble grid), do NOT apply
                // SFO phase correction — it distorts sync-word bins and
                // header decode.  The actual SFO slope is saved and
                // applied later in the SFD re-demod loop where it helps
                // the payload decode track timing drift.
                estimated_sfo = freq_est.sfo_slope;
                demod.set_frequency_offsets(freq_est.cfo_frac,
                                            freq_est.cfo_int,
                                            0.0f);
                demod_ref.set_frequency_offsets(freq_est.cfo_frac,
                                                freq_est.cfo_int,
                                                0.0f);
                demod.reset_symbol_counter();
                // Per-symbol CFO tracking (EMA).
                // --cfo-track [alpha] CLI flag or HOST_SIM_CFO_TRACK_ALPHA env.
                {
                    float alpha = options.cfo_track_alpha;
                    if (alpha == 0.0f) {
                        static const char* alpha_env = std::getenv("HOST_SIM_CFO_TRACK_ALPHA");
                        if (alpha_env) alpha = std::stof(alpha_env);
                    }
                    if (alpha > 0.0f) {
                        demod.set_cfo_tracking(alpha, 8);
                    }
                }
                std::cout << "Frequency offsets: CFO_int=" << freq_est.cfo_int
                          << " CFO_frac=" << freq_est.cfo_frac
                          << " SFO_slope=" << freq_est.sfo_slope << "\n";
            } else {
                demod.set_frequency_offsets(0.0f, 0, 0.0f);
                demod_ref.set_frequency_offsets(0.0f, 0.0f, 0);
                demod.reset_symbol_counter();
            }

            const int symbol_count = static_cast<int>(std::min<std::size_t>(
                (samples.size() > alignment_samples
                     ? (samples.size() - alignment_samples) / static_cast<std::size_t>(sps)
                     : 0),
                static_cast<std::size_t>(INT_MAX)));
            symbols.reserve(symbol_count);
            for (int idx = 0; idx < symbol_count; ++idx) {
                uint16_t value = demod.demodulate(&samples[alignment_samples + static_cast<std::size_t>(idx) * sps]);
                symbols.push_back(value);
                if (options.soft) {
                    const auto& mags = demod.get_fft_magnitudes_sq();
                    symbol_llrs.push_back(host_sim::compute_symbol_llrs(
                        mags.data(), metadata->sf,
                        (idx < 8) || metadata->ldro,
                        demod.current_cfo_int()));
                }
                uint16_t ref_value = demod_ref.demodulate(&samples[alignment_samples + static_cast<std::size_t>(idx) * sps]);
                if (value != ref_value) {
                    ++reference_mismatches;
                    if (reference_mismatches <= 8) {
                        std::cout << "[reference] mismatch symbol " << idx
                                  << " host=" << value << " ref=" << ref_value << "\n";
                    }
                }
            }

            if (symbol_count == 0) {
                std::cout << "[reference] no symbols available for comparison\n";
            } else if (reference_mismatches > 0) {
                compare_failure = true;
                std::cout << "[reference] total mismatched symbols: " << reference_mismatches << "\n";
            } else {
                std::cout << "[reference] symbols match reference demodulator\n";
            }
            summary.reference_mismatches = reference_mismatches;

        std::cout << "First 16 aligned symbols:";
        for (int i = 0; i < std::min<int>(16, symbols.size()); ++i) {
            std::cout << ' ' << symbols[i];
        }
        std::cout << "\n";
        if (symbols.size() >= 32) {
            std::cout << "Aligned symbols (0-31):";
            for (int i = 0; i < 32; ++i) {
                std::cout << ' ' << symbols[i];
            }
            std::cout << "\n";
        }
            summary.preview_symbols.assign(
                symbols.begin(),
                symbols.begin() + std::min<std::size_t>(symbols.size(), static_cast<std::size_t>(32)));

            HeaderDecodeResult header;
            std::size_t symbol_cursor = 0;
            std::size_t chosen_offset = std::numeric_limits<std::size_t>::max();

            // At high oversampling (os > 4) or BW500-class captures
            // (os == 4 in non-comparison mode), the LoRa SFD's 0.25-symbol
            // offset shifts data symbols off the preamble grid.  Scanning
            // preamble-grid symbols for a header is futile and can yield
            // false positives from noise, so skip straight to the SFD
            // re-demod path.  Stage-comparison tests at os == 4 still need
            // the preamble-grid scan to stay bit-exact with the reference.
            const bool skip_grid_scan =
                (os > 4) || (os == 4 && !options.compare_root);
            if (!skip_grid_scan) {
            for (std::size_t candidate = 0; candidate + 8 <= symbols.size(); ++candidate) {
                auto candidate_header = try_decode_header(symbols, candidate, *metadata);
                if (!candidate_header.success) {
                    continue;
                }

                int header_len = candidate_header.payload_len;
                if (header_len <= 0) {
                    header_len = metadata->payload_len;
                }
                if (metadata->payload_len > 0 && header_len != metadata->payload_len) {
                    continue;
                }

                int header_cr = candidate_header.cr;
                if (header_cr <= 0) {
                    header_cr = metadata->cr;
                }
                if (metadata->cr > 0 && header_cr != metadata->cr) {
                    continue;
                }

                if (metadata->has_crc && !candidate_header.has_crc) {
                    continue;
                }

                header = std::move(candidate_header);
                symbol_cursor = candidate + header.consumed_symbols;
                chosen_offset = candidate;
                break;
            }
            } // !skip_grid_scan

            // Fallback: if header not found on the preamble grid, try
            // re-demodulating from the sync word position with a quarter-
            // symbol SFD offset.  The LoRa SFD is 2.25 downchirps, so data
            // symbols start at sync + 2 (sync) + 2.25 (SFD) = sync + 4.25
            // symbols.  At high oversampling this 0.25 offset shifts the bin
            // by N/4, making header decode impossible on the preamble grid.
            // At os=1, the 0.25 offset is still non-zero (sps/4 samples),
            // so this fallback is needed at ALL oversampling factors.
            // Exception: stage-comparison mode (--compare-root) needs the
            // preamble-grid decode to stay bit-exact with the reference.
            if (!header.success && !options.compare_root) {
                auto sync_pos = host_sim::find_header_symbol_index(
                    symbols, 0x12, metadata->sf);
                if (!sync_pos) {
                    sync_pos = host_sim::find_header_symbol_index(
                        symbols, 0x34, metadata->sf);
                }

                // Save demod state for re-demod attempts (needed by
                // both the native-OS and the OS=2-upsample fallback).
                // Use the estimated SFO (from the preamble analysis) for
                // re-demod; the main demod loop ran with sfo_slope=0.
                const float saved_cfo_frac = demod.current_cfo_frac();
                const int   saved_cfo_int  = demod.current_cfo_int();
                const float saved_sfo      = estimated_sfo;

                // SFO-compensated stride for SFD re-demod.  The demod's
                // phase-domain SFO correction is NOT used (sfo_slope=0):
                // instead we move the FFT window to the correct timing
                // position for each symbol via variable stride.  This
                // avoids the cumulative phase-model error that made the
                // old phase-domain correction unreliable.
                const int N_bins = 1 << metadata->sf;
                const double redemod_stride = (saved_sfo != 0.0f && !options.compare_root)
                    ? static_cast<double>(sps) * (1.0 - static_cast<double>(saved_sfo) / N_bins)
                    : static_cast<double>(sps);

                if (sync_pos) {
                    const std::size_t quarter = static_cast<std::size_t>(sps / 4);

                    // Try quarter-symbol offsets: 1 (standard), 0, 2, 3
                    for (int qoff : {1, 0, 2, 3}) {
                        if (header.success) break;
                        const std::size_t data_sample = alignment_samples +
                            *sync_pos * static_cast<std::size_t>(sps) +
                            static_cast<std::size_t>(qoff) * quarter;
                        if (data_sample + 8ULL * sps > samples.size()) continue;

                        // Re-demodulate data symbols from adjusted position.
                        // Use SFO stride for timing; sfo_slope=0 in demod.
                        demod.set_frequency_offsets(saved_cfo_frac,
                                                   saved_cfo_int,
                                                   0.0f);
                        demod.reset_symbol_counter();
                        std::vector<uint16_t> redemod;
                        std::vector<host_sim::SymbolLLR> redemod_llrs;
                        const std::size_t max_sym = (samples.size() - data_sample) / sps;
                        // Per-symbol SFO tracking: refine stride using
                        // residual drift from parabolic interpolation.
                        double sfo_accum = 0.0;
                        double sfo_stride = redemod_stride;
                        float sfo_prev_res = 0.0f;
                        float sfo_drift = 0.0f;
                        constexpr float sfo_alpha = 0.01f;
                        constexpr int sfo_delay = 8;
                        // Cap at 1024 symbols: enough for max LoRa payload (255B, any SF/CR)
                        // while preventing multi-packet mode from consuming the entire capture.
                        for (std::size_t i = 0; i < std::min<std::size_t>(max_sym, 1024); ++i) {
                            const std::size_t sym_off = static_cast<std::size_t>(
                                std::round(sfo_accum));
                            if (data_sample + sym_off + static_cast<std::size_t>(sps) > samples.size()) break;
                            redemod.push_back(demod.demodulate(
                                &samples[data_sample + sym_off]));
                            if (options.soft) {
                                const auto& mags = demod.get_fft_magnitudes_sq();
                                redemod_llrs.push_back(host_sim::compute_symbol_llrs(
                                    mags.data(), metadata->sf,
                                    (static_cast<int>(i) < 8) || metadata->ldro,
                                    saved_cfo_int));
                            }
                            // Adaptive stride: track residual slope.
                            if (static_cast<int>(i) >= sfo_delay) {
                                float dr = demod.last_residual() - sfo_prev_res;
                                if (dr > 0.5f) dr -= 1.0f;
                                if (dr < -0.5f) dr += 1.0f;
                                sfo_drift += sfo_alpha * (dr - sfo_drift);
                                sfo_stride = redemod_stride -
                                    static_cast<double>(sps) *
                                    static_cast<double>(sfo_drift) / N_bins;
                            }
                            sfo_prev_res = demod.last_residual();
                            sfo_accum += sfo_stride;
                        }

                        if (metadata->implicit_header) {
                            // Helper lambda: build implicit header from symbols.
                            auto build_implicit_header = [&](const std::vector<uint16_t>& syms)
                                -> HeaderDecodeResult {
                                HeaderDecodeResult h;
                                const std::size_t hdr_syms = std::min<std::size_t>(8, syms.size());
                                std::vector<uint16_t> first_block(syms.begin(),
                                                                  syms.begin() + hdr_syms);
                                host_sim::DeinterleaverConfig hdr_cfg{metadata->sf, 4, true, metadata->ldro};
                                std::size_t consumed_block = 0;
                                auto codewords = host_sim::deinterleave(first_block, hdr_cfg, consumed_block);
                                auto nibs = host_sim::hamming_decode_block(codewords, true, 4);
                                h.success = true;
                                h.payload_len = metadata->payload_len;
                                h.cr = metadata->cr;
                                h.has_crc = metadata->has_crc;
                                h.checksum_field = -1;
                                h.checksum_computed = -1;
                                h.consumed_symbols = consumed_block > 0 ? consumed_block : 8;
                                h.codewords.assign(codewords.begin(), codewords.end());
                                h.nibbles.assign(5, 0);
                                h.nibbles.insert(h.nibbles.end(), nibs.begin(), nibs.end());
                                return h;
                            };

                            auto imp_hdr = build_implicit_header(redemod);

                            // CRC-guided timing sweep for implicit header.
                            if (metadata->has_crc &&
                                !probe_payload_crc(redemod, imp_hdr, *metadata)) {
                                const int max_adj = std::max(os, 4) + 2;
                                for (int adj = -1; std::abs(adj) <= max_adj;
                                     adj = adj > 0 ? -adj - 1 : -adj) {
                                    const auto adj_data = static_cast<std::size_t>(
                                        static_cast<std::ptrdiff_t>(data_sample) + adj);
                                    if (adj_data + 8ULL * sps > samples.size())
                                        continue;
                                    demod.set_frequency_offsets(saved_cfo_frac,
                                                               saved_cfo_int,
                                                               0.0f);
                                    demod.reset_symbol_counter();
                                    std::vector<uint16_t> adj_syms;
                                    std::vector<host_sim::SymbolLLR> adj_llrs;
                                    const std::size_t adj_max =
                                        (samples.size() - adj_data) / sps;
                                    for (std::size_t i = 0;
                                         i < std::min<std::size_t>(adj_max, 200);
                                         ++i) {
                                        const std::size_t sym_off = static_cast<std::size_t>(
                                            std::round(i * redemod_stride));
                                        if (adj_data + sym_off + static_cast<std::size_t>(sps) > samples.size()) break;
                                        adj_syms.push_back(demod.demodulate(
                                            &samples[adj_data + sym_off]));
                                        if (options.soft) {
                                            const auto& mags = demod.get_fft_magnitudes_sq();
                                            adj_llrs.push_back(host_sim::compute_symbol_llrs(
                                                mags.data(), metadata->sf,
                                                (static_cast<int>(i) < 8) || metadata->ldro,
                                                saved_cfo_int));
                                        }
                                    }
                                    auto adj_hdr = build_implicit_header(adj_syms);
                                    if (probe_payload_crc(adj_syms, adj_hdr,
                                                          *metadata)) {
                                        redemod = std::move(adj_syms);
                                        redemod_llrs = std::move(adj_llrs);
                                        imp_hdr = std::move(adj_hdr);
                                        std::cout << "SFD re-demod: implicit data start "
                                                     "refined by "
                                                  << adj
                                                  << " samples (CRC verified)\n";
                                        break;
                                    }
                                }
                            }

                            // Only accept this qoff if CRC validated
                            // (or if there's no CRC to check).
                            bool crc_ok = !metadata->has_crc ||
                                probe_payload_crc(redemod, imp_hdr, *metadata);

                            if (crc_ok) {
                                std::cout << "SFD re-demod: implicit header, quarter offset "
                                          << qoff << " (sync at symbol " << (*sync_pos - 4) << ")\n";
                                header = std::move(imp_hdr);
                                symbol_cursor = header.consumed_symbols;
                                chosen_offset = 0;
                                data_start_sample = data_sample;
                                symbols = std::move(redemod);
                                symbol_llrs = std::move(redemod_llrs);
                                break;
                            }
                            // CRC failed even after timing sweep — try next qoff.
                            continue;
                        }

                        auto hdr = try_decode_header(redemod, 0, *metadata);
                        if (!hdr.success) continue;

                        int hlen = hdr.payload_len > 0 ? hdr.payload_len : metadata->payload_len;
                        int hcr = hdr.cr > 0 ? hdr.cr : metadata->cr;
                        if (metadata->payload_len > 0 && hlen != metadata->payload_len) continue;
                        if (metadata->cr > 0 && hcr != metadata->cr) continue;
                        if (metadata->has_crc && !hdr.has_crc) continue;

                        // SFO-induced timing drift between the preamble
                        // and data start can shift data symbols by ±1 bin.
                        // The header is robust (uses sf_app = sf-2) but the
                        // payload is not.  Sweep small timing adjustments
                        // around the nominal data_sample and select the one
                        // that gives a valid CRC.
                        if ((hdr.has_crc || metadata->has_crc) &&
                            !probe_payload_crc(redemod, hdr, *metadata)) {
                            const int max_adj = std::max(os, 4) + 2;
                            for (int adj = -1; std::abs(adj) <= max_adj; adj = adj > 0 ? -adj - 1 : -adj) {
                                const auto adj_data = static_cast<std::size_t>(
                                    static_cast<std::ptrdiff_t>(data_sample) + adj);
                                if (adj_data + 8ULL * sps > samples.size())
                                    continue;
                                demod.set_frequency_offsets(saved_cfo_frac,
                                                           saved_cfo_int,
                                                           0.0f);
                                demod.reset_symbol_counter();
                                std::vector<uint16_t> adj_syms;
                                std::vector<host_sim::SymbolLLR> adj_llrs;
                                const std::size_t adj_max =
                                    (samples.size() - adj_data) / sps;
                                for (std::size_t i = 0;
                                     i < std::min<std::size_t>(adj_max, 200);
                                     ++i) {
                                    const std::size_t sym_off = static_cast<std::size_t>(
                                        std::round(i * redemod_stride));
                                    if (adj_data + sym_off + static_cast<std::size_t>(sps) > samples.size()) break;
                                    adj_syms.push_back(demod.demodulate(
                                        &samples[adj_data + sym_off]));
                                    if (options.soft) {
                                        const auto& mags = demod.get_fft_magnitudes_sq();
                                        adj_llrs.push_back(host_sim::compute_symbol_llrs(
                                            mags.data(), metadata->sf,
                                            (static_cast<int>(i) < 8) || metadata->ldro,
                                            saved_cfo_int));
                                    }
                                }
                                auto adj_hdr =
                                    try_decode_header(adj_syms, 0, *metadata);
                                if (!adj_hdr.success) continue;
                                if (probe_payload_crc(adj_syms, adj_hdr,
                                                      *metadata)) {
                                    redemod = std::move(adj_syms);
                                    redemod_llrs = std::move(adj_llrs);
                                    hdr = std::move(adj_hdr);
                                    std::cout << "SFD re-demod: data start "
                                                 "refined by "
                                              << adj
                                              << " samples (CRC verified)\n";
                                    break;
                                }
                            }
                        }

                        std::cout << "SFD re-demod: header found with quarter offset "
                                  << qoff << " (sync at symbol " << (*sync_pos - 4)
                                  << ")\n";
                        header = std::move(hdr);
                        symbol_cursor = header.consumed_symbols;
                        chosen_offset = 0;
                        data_start_sample = data_sample;
                        symbols = std::move(redemod);
                        symbol_llrs = std::move(redemod_llrs);
                        break;
                    }
                }

                // ── OS=2 upsample fallback ──────────────────────────
                // When the native-OS decode at OS=1 either fails or
                // produces a CRC-invalid payload, upsample the burst by
                // 2x and retry.  This doubles the SFO timing tolerance
                // (n_symbols × sfo_ppm threshold ~4000 vs ~2000 at OS=1).
                // If OS=2 also fails CRC, the original decode is kept.
                bool need_os2 = !header.success;
                if (!need_os2 && os == 1 && sync_pos &&
                    (header.has_crc || metadata->has_crc) &&
                    !probe_payload_crc(symbols, header, *metadata)) {
                    need_os2 = true;
                }
                if (need_os2 && sync_pos && os == 1) {
                    // Save current decode as fallback.
                    auto fallback_header = header;
                    auto fallback_symbols = symbols;
                    auto fallback_llrs = symbol_llrs;
                    auto fallback_cursor = symbol_cursor;
                    auto fallback_offset = chosen_offset;
                    header.success = false;

                    const std::size_t burst_start = alignment_samples;
                    const std::size_t burst_len = samples.size() - burst_start;
                    auto up = upsample_2x(&samples[burst_start], burst_len);

                    const int sps_os2 = sps * 2;
                    const std::size_t quarter_os2 =
                        static_cast<std::size_t>(sps_os2 / 4);
                    host_sim::FftDemodulator demod_os2(metadata->sf,
                                            metadata->bw * 2,
                                            metadata->bw);

                    // Sweep SFO rate compensation: 0 first, then spiral
                    // outward.  SFO shifts symbol boundaries by
                    //   drift_per_sym = sfo_ppm * sps_os2 / 1e6
                    // We compensate by adjusting the stride between
                    // successive FFT windows.
                    //
                    // Two passes: pass 0 skips timing refinement (fast
                    // reject of wrong SFO candidates), pass 1 adds ±6
                    // sample adjustments for borderline alignments.
                    // Track which (sfo_cand, qoff) passed header on
                    // pass 0 so pass 1 only retries those.
                    std::vector<std::pair<int,int>> hdr_hit_pairs;
                    for (int os2_pass = 0;
                         os2_pass < 2 && !header.success; ++os2_pass) {
                    for (int sfo_cand = 0; std::abs(sfo_cand) <= 100;
                         sfo_cand = sfo_cand >= 0 ? -sfo_cand - 10
                                                  : -sfo_cand) {
                        if (header.success) break;
                        const double stride =
                            static_cast<double>(sps_os2) *
                            (1.0 - static_cast<double>(sfo_cand) * 1e-6);

                    for (int qoff : {1, 0, 2, 3}) {
                        if (header.success) break;
                        // Fast pass: only try qoff=1 (most common),
                        // skip timing-adjustment loop.
                        if (os2_pass == 0 && qoff != 1) continue;
                        // Full pass: only retry pairs that passed
                        // header on pass 0 (need adj refinement).
                        if (os2_pass == 1) {
                            bool was_hit = false;
                            for (auto& p : hdr_hit_pairs)
                                if (p.first == sfo_cand && p.second == qoff)
                                    was_hit = true;
                            if (!was_hit) continue;
                        }
                        const std::size_t data_sample_os2 =
                            *sync_pos * static_cast<std::size_t>(sps_os2) +
                            static_cast<std::size_t>(qoff) * quarter_os2;
                        if (data_sample_os2 + 8ULL * sps_os2 > up.size())
                            continue;

                        demod_os2.set_frequency_offsets(saved_cfo_frac,
                                                       saved_cfo_int,
                                                       0.0f);
                        demod_os2.reset_symbol_counter();
                        std::vector<uint16_t> redemod;
                        std::vector<host_sim::SymbolLLR> redemod_llrs;

                        // Phase 1: demod first 8 symbols for header probe
                        for (std::size_t i = 0; i < 8; ++i) {
                            const auto pos = static_cast<std::size_t>(
                                std::round(static_cast<double>(
                                               data_sample_os2) +
                                           static_cast<double>(i) * stride));
                            if (pos + static_cast<std::size_t>(sps_os2) >
                                up.size())
                                break;
                            redemod.push_back(demod_os2.demodulate(&up[pos]));
                            if (options.soft) {
                                const auto& mags = demod_os2.get_fft_magnitudes_sq();
                                redemod_llrs.push_back(
                                    host_sim::compute_symbol_llrs(
                                        mags.data(), metadata->sf,
                                        true,
                                        saved_cfo_int));
                            }
                        }

                        // Early header check — skip full demod when
                        // header clearly wrong (explicit header only).
                        std::size_t max_syms_needed = 1024;
                        // For implicit header, compute cap from metadata.
                        if (metadata->implicit_header) {
                            if (metadata->payload_len > 0 &&
                                metadata->cr > 0) {
                                const std::size_t nibbles =
                                    static_cast<std::size_t>(
                                        metadata->payload_len) * 2 + 4;
                                const std::size_t cw =
                                    static_cast<std::size_t>(
                                        metadata->cr + 4);
                                const std::size_t blocks =
                                    (nibbles + cw - 1) / cw;
                                max_syms_needed = 8 + blocks * cw + 4;
                            }
                        } else {
                            auto hdr_probe =
                                try_decode_header(redemod, 0, *metadata);
                            if (!hdr_probe.success) continue;
                            int hlen_p = hdr_probe.payload_len > 0
                                             ? hdr_probe.payload_len
                                             : metadata->payload_len;
                            int hcr_p = hdr_probe.cr > 0
                                            ? hdr_probe.cr
                                            : metadata->cr;
                            if (metadata->payload_len > 0 &&
                                hlen_p != metadata->payload_len)
                                continue;
                            if (metadata->cr > 0 &&
                                hcr_p != metadata->cr)
                                continue;
                            if (metadata->has_crc && !hdr_probe.has_crc)
                                continue;
                            // Cap demod to symbols needed for CRC check:
                            // header_consumed + ceil(nibbles / cw_len) * cw_len + margin.
                            if (hlen_p > 0 && hcr_p > 0) {
                                const std::size_t nibbles =
                                    static_cast<std::size_t>(hlen_p) * 2 + 4;
                                const std::size_t cw = static_cast<std::size_t>(hcr_p + 4);
                                const std::size_t blocks = (nibbles + cw - 1) / cw;
                                const std::size_t hdr_sym =
                                    hdr_probe.consumed_symbols > 0
                                        ? hdr_probe.consumed_symbols : 8;
                                max_syms_needed = hdr_sym + blocks * cw + 4;
                            }
                        }

                        // Phase 2: demod remaining symbols
                        for (std::size_t i = redemod.size();
                             i < max_syms_needed; ++i) {
                            const auto pos = static_cast<std::size_t>(
                                std::round(static_cast<double>(
                                               data_sample_os2) +
                                           static_cast<double>(i) * stride));
                            if (pos + static_cast<std::size_t>(sps_os2) >
                                up.size())
                                break;
                            redemod.push_back(demod_os2.demodulate(&up[pos]));
                            if (options.soft) {
                                const auto& mags = demod_os2.get_fft_magnitudes_sq();
                                redemod_llrs.push_back(
                                    host_sim::compute_symbol_llrs(
                                        mags.data(), metadata->sf,
                                        (static_cast<int>(i) < 8) ||
                                            metadata->ldro,
                                        saved_cfo_int));
                            }
                        }

                        if (metadata->implicit_header) {
                            HeaderDecodeResult imp_hdr;
                            {
                                const std::size_t hdr_syms =
                                    std::min<std::size_t>(8, redemod.size());
                                std::vector<uint16_t> first_block(
                                    redemod.begin(),
                                    redemod.begin() + hdr_syms);
                                host_sim::DeinterleaverConfig hdr_cfg{
                                    metadata->sf, 4, true, metadata->ldro};
                                std::size_t consumed_block = 0;
                                auto codewords = host_sim::deinterleave(
                                    first_block, hdr_cfg, consumed_block);
                                auto nibs =
                                    host_sim::hamming_decode_block(
                                        codewords, true, 4);
                                imp_hdr.success = true;
                                imp_hdr.payload_len = metadata->payload_len;
                                imp_hdr.cr = metadata->cr;
                                imp_hdr.has_crc = metadata->has_crc;
                                imp_hdr.checksum_field = -1;
                                imp_hdr.checksum_computed = -1;
                                imp_hdr.consumed_symbols =
                                    consumed_block > 0 ? consumed_block : 8;
                                imp_hdr.codewords.assign(
                                    codewords.begin(), codewords.end());
                                imp_hdr.nibbles.assign(5, 0);
                                imp_hdr.nibbles.insert(
                                    imp_hdr.nibbles.end(),
                                    nibs.begin(), nibs.end());
                            }

                            if (metadata->has_crc &&
                                !probe_payload_crc(redemod, imp_hdr,
                                                   *metadata)) {
                                if (os2_pass == 0) {
                                    hdr_hit_pairs.push_back({sfo_cand, qoff});
                                    continue;
                                }
                                for (int adj = -1; std::abs(adj) <= 3;
                                     adj = adj > 0 ? -adj - 1 : -adj) {
                                    const auto adj_data =
                                        static_cast<std::size_t>(
                                            static_cast<std::ptrdiff_t>(
                                                data_sample_os2) +
                                            adj);
                                    if (adj_data + 8ULL * sps_os2 >
                                        up.size())
                                        continue;
                                    demod_os2.set_frequency_offsets(
                                        saved_cfo_frac, saved_cfo_int,
                                        0.0f);
                                    demod_os2.reset_symbol_counter();
                                    std::vector<uint16_t> adj_syms;
                                    for (std::size_t i = 0; i < max_syms_needed; ++i) {
                                        const auto pos = static_cast<std::size_t>(
                                            std::round(static_cast<double>(
                                                           adj_data) +
                                                       static_cast<double>(i) * stride));
                                        if (pos + static_cast<std::size_t>(sps_os2) >
                                            up.size())
                                            break;
                                        adj_syms.push_back(
                                            demod_os2.demodulate(&up[pos]));
                                    }
                                    HeaderDecodeResult adj_imp;
                                    {
                                        const std::size_t hs =
                                            std::min<std::size_t>(
                                                8, adj_syms.size());
                                        std::vector<uint16_t> fb(
                                            adj_syms.begin(),
                                            adj_syms.begin() + hs);
                                        host_sim::DeinterleaverConfig
                                            hcfg{metadata->sf, 4, true,
                                                 metadata->ldro};
                                        std::size_t cb = 0;
                                        auto cw =
                                            host_sim::deinterleave(
                                                fb, hcfg, cb);
                                        auto nb =
                                            host_sim::hamming_decode_block(
                                                cw, true, 4);
                                        adj_imp.success = true;
                                        adj_imp.payload_len =
                                            metadata->payload_len;
                                        adj_imp.cr = metadata->cr;
                                        adj_imp.has_crc =
                                            metadata->has_crc;
                                        adj_imp.checksum_field = -1;
                                        adj_imp.checksum_computed = -1;
                                        adj_imp.consumed_symbols =
                                            cb > 0 ? cb : 8;
                                        adj_imp.codewords.assign(
                                            cw.begin(), cw.end());
                                        adj_imp.nibbles.assign(5, 0);
                                        adj_imp.nibbles.insert(
                                            adj_imp.nibbles.end(),
                                            nb.begin(), nb.end());
                                    }
                                    if (probe_payload_crc(adj_syms, adj_imp,
                                                          *metadata)) {
                                        redemod = std::move(adj_syms);
                                        imp_hdr = std::move(adj_imp);
                                        std::cout
                                            << "OS=2 upsample: implicit "
                                               "data start refined by "
                                            << adj
                                            << " samples (CRC verified)\n";
                                        break;
                                    }
                                }
                            }

                            bool crc_ok = !metadata->has_crc ||
                                probe_payload_crc(redemod, imp_hdr,
                                                  *metadata);
                            if (crc_ok) {
                                std::cout
                                    << "OS=2 upsample: implicit header, "
                                       "quarter offset "
                                    << qoff;
                                if (sfo_cand != 0) {
                                    std::cout << " (SFO=" << sfo_cand << "ppm)";
                                }
                                std::cout << "\n";
                                header = std::move(imp_hdr);
                                symbol_cursor = header.consumed_symbols;
                                chosen_offset = 0;
                                data_start_sample =
                                    alignment_samples + data_sample_os2 / 2;
                                symbols = std::move(redemod);
                                symbol_llrs = std::move(redemod_llrs);
                                break;
                            }
                            continue;
                        }

                        auto hdr_os2 =
                            try_decode_header(redemod, 0, *metadata);
                        if (!hdr_os2.success) continue;

                        int hlen = hdr_os2.payload_len > 0
                                       ? hdr_os2.payload_len
                                       : metadata->payload_len;
                        int hcr = hdr_os2.cr > 0 ? hdr_os2.cr
                                                  : metadata->cr;
                        if (metadata->payload_len > 0 &&
                            hlen != metadata->payload_len)
                            continue;
                        if (metadata->cr > 0 && hcr != metadata->cr)
                            continue;
                        if (metadata->has_crc && !hdr_os2.has_crc)
                            continue;

                        if ((hdr_os2.has_crc || metadata->has_crc) &&
                            !probe_payload_crc(redemod, hdr_os2,
                                               *metadata)) {
                            if (os2_pass == 0) {
                                hdr_hit_pairs.push_back({sfo_cand, qoff});
                                continue;
                            }
                            for (int adj = -1; std::abs(adj) <= 3;
                                 adj = adj > 0 ? -adj - 1 : -adj) {
                                const auto adj_data =
                                    static_cast<std::size_t>(
                                        static_cast<std::ptrdiff_t>(
                                            data_sample_os2) +
                                        adj);
                                if (adj_data + 8ULL * sps_os2 > up.size())
                                    continue;
                                demod_os2.set_frequency_offsets(
                                    saved_cfo_frac, saved_cfo_int, 0.0f);
                                demod_os2.reset_symbol_counter();
                                std::vector<uint16_t> adj_syms;
                                for (std::size_t i = 0; i < max_syms_needed; ++i) {
                                    const auto pos = static_cast<std::size_t>(
                                        std::round(static_cast<double>(
                                                       adj_data) +
                                                   static_cast<double>(i) * stride));
                                    if (pos + static_cast<std::size_t>(sps_os2) >
                                        up.size())
                                        break;
                                    adj_syms.push_back(
                                        demod_os2.demodulate(&up[pos]));
                                }
                                auto adj_hdr = try_decode_header(
                                    adj_syms, 0, *metadata);
                                if (!adj_hdr.success) continue;
                                if (probe_payload_crc(adj_syms, adj_hdr,
                                                      *metadata)) {
                                    redemod = std::move(adj_syms);
                                    hdr_os2 = std::move(adj_hdr);
                                    std::cout
                                        << "OS=2 upsample: data start "
                                           "refined by "
                                        << adj
                                        << " samples (CRC verified)\n";
                                    break;
                                }
                            }
                        }

                        // Only accept if CRC passes (or no CRC).
                        // If CRC fails, try the next SFO candidate.
                        if ((hdr_os2.has_crc || metadata->has_crc) &&
                            !probe_payload_crc(redemod, hdr_os2,
                                               *metadata)) {
                            continue;
                        }

                        std::cout << "OS=2 upsample: header found with "
                                     "quarter offset "
                                  << qoff;
                        if (sfo_cand != 0) {
                            std::cout << " (SFO=" << sfo_cand << "ppm)";
                        }
                        std::cout << "\n";
                        header = std::move(hdr_os2);
                        symbol_cursor = header.consumed_symbols;
                        chosen_offset = 0;
                        data_start_sample =
                            alignment_samples + data_sample_os2 / 2;
                        symbols = std::move(redemod);
                        symbol_llrs = std::move(redemod_llrs);
                        break;
                    }
                    } // sfo_cand
                    } // os2_pass

                    // If OS=2 didn't produce CRC-valid decode, restore
                    // the original native-OS result.
                    if (!header.success && fallback_header.success) {
                        header = std::move(fallback_header);
                        symbols = std::move(fallback_symbols);
                        symbol_llrs = std::move(fallback_llrs);
                        symbol_cursor = fallback_cursor;
                        chosen_offset = fallback_offset;
                    }
                }
            }

            if (header.success) {
                std::cout << "Header located at symbol index: " << chosen_offset << "\n";
                std::cout << "Header (nibbles):";
                for (auto nib : header.nibbles) {
                    std::cout << ' ' << std::hex << static_cast<int>(nib) << std::dec;
                }
                std::cout << "\n";
                std::cout << "Decoded header -> payload_len=" << header.payload_len
                          << " crc=" << header.has_crc
                          << " cr=" << header.cr
                          << " checksum_field=" << header.checksum_field
                          << " checksum_calc=" << header.checksum_computed << "\n";

                int expected_payload_len = header.payload_len > 0 ? header.payload_len : metadata->payload_len;
                bool expected_crc = header.has_crc;
                if (metadata->has_crc && !header.has_crc) {
                    expected_crc = true;
                }
                const int active_cr = header.cr > 0 ? header.cr : metadata->cr;

                const std::size_t header_symbol_span = std::min<std::size_t>(
                    header.consumed_symbols > 0 ? header.consumed_symbols : static_cast<std::size_t>(8),
                    symbols.size() - chosen_offset);
                std::vector<uint16_t> header_block(symbols.begin() + chosen_offset,
                                                   symbols.begin() + chosen_offset + header_symbol_span);
                host_sim::DeinterleaverConfig header_cfg{metadata->sf, 4, true, metadata->ldro};

                have_stage_outputs = true;
                const int header_stage_symbols = header_cfg.is_header ? 8 : header_cfg.cr + 4;
                const std::size_t used_symbols =
                    std::min<std::size_t>(static_cast<std::size_t>(header_stage_symbols), header_block.size());
                std::vector<uint16_t> header_block_stage(header_block.begin(),
                                                         header_block.begin() + used_symbols);
                append_fft_gray(header_block_stage, true, metadata->ldro, metadata->sf, stage_outputs);

                const std::size_t stage_codewords = std::min<std::size_t>(
                    header.codewords.size(),
                    static_cast<std::size_t>(std::max(metadata->sf - 2, 0)));
                for (std::size_t i = 0; i < stage_codewords; ++i) {
                    stage_outputs.deinterleaver.push_back(static_cast<uint16_t>(header.codewords[i]));
                }

                for (std::size_t i = 0; i < stage_codewords && i < header.nibbles.size(); ++i) {
                    stage_outputs.hamming.push_back(static_cast<uint8_t>(header.nibbles[i] & 0xF));
                }

                std::size_t nibble_target = static_cast<std::size_t>(expected_payload_len) * 2;
                if (expected_crc) {
                    nibble_target += 4; // 2 bytes CRC
                }

                host_sim::DeinterleaverConfig payload_cfg{metadata->sf, active_cr, false, metadata->ldro};
                std::vector<uint8_t> payload_nibbles;

                // For SF >= 8, the header block (8 symbols at CR=4/8)
                // produces SF-2 nibbles: the first 5 are header fields,
                // and the remaining SF-7 are the start of the payload.
                // These must be prepended to the payload nibble stream.
                if (header.nibbles.size() > 5) {
                    for (std::size_t i = 5; i < header.nibbles.size(); ++i) {
                        payload_nibbles.push_back(header.nibbles[i]);
                    }
                }

                const int payload_cw_len = active_cr + 4;
                const bool suppress_payload_stage = (metadata->sf <= 6);
                while (symbol_cursor + payload_cw_len <= symbols.size() && payload_nibbles.size() < nibble_target) {
                    std::vector<uint16_t> block(symbols.begin() + symbol_cursor,
                                                symbols.begin() + symbol_cursor + payload_cw_len);
                    std::size_t consumed_block = 0;

                    std::vector<uint8_t> nibs;
                    std::vector<uint8_t> codewords;
                    if (options.soft && symbol_cursor + payload_cw_len <= symbol_llrs.size()) {
                        std::vector<host_sim::SymbolLLR> block_llrs(
                            symbol_llrs.begin() + symbol_cursor,
                            symbol_llrs.begin() + symbol_cursor + payload_cw_len);
                        nibs = host_sim::soft_decode_block(
                            block_llrs, metadata->sf, active_cr,
                            false, metadata->ldro, consumed_block);
                        // Hard deinterleave for stage outputs
                        codewords = host_sim::deinterleave(block, payload_cfg, consumed_block);
                    } else {
                        codewords = host_sim::deinterleave(block, payload_cfg, consumed_block);
                        nibs = host_sim::hamming_decode_block(codewords, false, active_cr);
                    }

                    if (consumed_block == 0) {
                        break;
                    }
                    symbol_cursor += consumed_block;
                    if (!suppress_payload_stage) {
                        append_fft_gray(block, false, metadata->ldro, metadata->sf, stage_outputs);
                    }
                    payload_nibbles.insert(payload_nibbles.end(), nibs.begin(), nibs.end());
                    if (!suppress_payload_stage) {
                        for (auto cw : codewords) {
                            stage_outputs.deinterleaver.push_back(static_cast<uint16_t>(cw));
                        }
                        for (auto nib : nibs) {
                            stage_outputs.hamming.push_back(static_cast<uint8_t>(nib & 0xF));
                        }
                    }
                }

                // Dewhiten nibbles first, then pack into bytes (matching GNU Radio's approach)
                // In gr-lora_sdr dewhitening_impl.cc:
                //   low_nib = in[2*i] ^ (whitening_seq[offset] & 0x0F);
                //   high_nib = in[2*i+1] ^ (whitening_seq[offset] >> 4);
                //   byte = high_nib << 4 | low_nib;
                // Note: nibble order is: [0]=low_nib, [1]=high_nib
                host_sim::WhiteningSequencer seq;
                auto whitening = seq.sequence(payload_nibbles.size() / 2);
                
                std::vector<uint8_t> unwhitened;
                unwhitened.reserve(payload_nibbles.size() / 2);
                for (std::size_t i = 0; i + 1 < payload_nibbles.size(); i += 2) {
                    std::size_t byte_idx = i / 2;
                    // GNU Radio convention: first nibble is low_nib, second is high_nib
                    // Note: CRC bytes are NOT dewhitened (matching GNU Radio behavior)
                    uint8_t low_nib, high_nib;
                    if (byte_idx < static_cast<std::size_t>(expected_payload_len)) {
                        // Payload bytes: apply dewhitening
                        uint8_t w = (byte_idx < whitening.size()) ? whitening[byte_idx] : 0;
                        low_nib = (payload_nibbles[i] & 0xF) ^ (w & 0x0F);
                        high_nib = (payload_nibbles[i + 1] & 0xF) ^ ((w >> 4) & 0x0F);
                    } else {
                        // CRC bytes: no dewhitening
                        low_nib = (payload_nibbles[i] & 0xF);
                        high_nib = (payload_nibbles[i + 1] & 0xF);
                    }
                    uint8_t byte = static_cast<uint8_t>((high_nib << 4) | low_nib);
                    unwhitened.push_back(byte);
                }
                if (unwhitened.size() > static_cast<std::size_t>(expected_payload_len) + (expected_crc ? 2u : 0u)) {
                    unwhitened.resize(static_cast<std::size_t>(expected_payload_len) + (expected_crc ? 2u : 0u));
                }
                std::cout << "Payload bytes (dewhitened):";
                for (std::size_t i = 0; i < std::min<std::size_t>(unwhitened.size(), static_cast<std::size_t>(expected_payload_len)); ++i) {
                    std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                              << static_cast<int>(unwhitened[i]) << std::dec;
                }
                std::cout << "\n";
                if (expected_payload_len > 0) {
                    std::string ascii(unwhitened.begin(), unwhitened.begin() + std::min<std::size_t>(static_cast<std::size_t>(expected_payload_len), unwhitened.size()));
                    std::cout << "Payload ASCII: " << ascii << "\n";
                }
                if (!options.payload.empty() && expected_payload_len > 0) {
                    bool payload_match = true;
                    const std::string& expected_payload = options.payload;
                    if (expected_payload.size() != static_cast<std::size_t>(expected_payload_len)) {
                        payload_match = false;
                        std::cout << "[payload] expected length " << expected_payload.size()
                                  << " does not match decoded length "
                                  << expected_payload_len << "\n";
                    } else if (unwhitened.size() < static_cast<std::size_t>(expected_payload_len) ||
                               !std::equal(expected_payload.begin(), expected_payload.end(),
                                           unwhitened.begin())) {
                        payload_match = false;
                        std::cout << "[payload] decoded payload differs from expected string\n";
                    }
                    if (!payload_match) {
                        payload_failure = true;
                    }
                } else if (options.payload.empty() && metadata->payload_hex &&
                           !metadata->payload_hex->empty() && expected_payload_len > 0) {
                    // Verify against payload_hex from metadata
                    const std::string& hex_str = *metadata->payload_hex;
                    bool payload_match = true;
                    if (hex_str.size() != static_cast<std::size_t>(expected_payload_len) * 2) {
                        payload_match = false;
                        std::cout << "[payload] payload_hex length " << hex_str.size() / 2
                                  << " does not match decoded length "
                                  << expected_payload_len << "\n";
                    } else if (unwhitened.size() >= static_cast<std::size_t>(expected_payload_len)) {
                        for (int i = 0; i < expected_payload_len; ++i) {
                            unsigned int expected_byte = 0;
                            auto [ptr, ec] = std::from_chars(
                                hex_str.data() + i * 2, hex_str.data() + i * 2 + 2,
                                expected_byte, 16);
                            if (ec != std::errc{} || unwhitened[i] != static_cast<uint8_t>(expected_byte)) {
                                payload_match = false;
                                std::cout << "[payload] payload_hex mismatch at byte " << i << "\n";
                                break;
                            }
                        }
                    } else {
                        payload_match = false;
                    }
                    if (!payload_match) {
                        payload_failure = true;
                    }
                }

                if (expected_crc && expected_payload_len >= 2 && unwhitened.size() >= static_cast<std::size_t>(expected_payload_len) + 2) {
                    // GNU Radio CRC verification algorithm:
                    // 1. Compute CRC on first (payload_len - 2) bytes
                    // 2. XOR with last 2 payload bytes
                    // 3. Compare with received CRC (little-endian)
                    std::vector<uint8_t> crc_data(unwhitened.begin(),
                                                  unwhitened.begin() + expected_payload_len - 2);
                    uint16_t computed_crc = compute_raw_crc16(crc_data);
                    // XOR with last 2 payload bytes
                    if (expected_payload_len >= 2) {
                        computed_crc ^= unwhitened[expected_payload_len - 1];  // last byte
                        computed_crc ^= static_cast<uint16_t>(unwhitened[expected_payload_len - 2]) << 8;  // second-to-last byte
                    }
                    const uint8_t crc_byte0 = unwhitened[expected_payload_len];
                    const uint8_t crc_byte1 = unwhitened[expected_payload_len + 1];
                    const uint16_t decoded_crc = static_cast<uint16_t>(crc_byte0) |
                                                 (static_cast<uint16_t>(crc_byte1) << 8);
                    const bool crc_ok = (computed_crc == decoded_crc);
                    std::cout << "[payload] CRC decoded=0x" << std::hex << std::setw(4)
                              << std::setfill('0') << decoded_crc
                              << " computed=0x" << std::setw(4) << computed_crc
                              << std::dec << (crc_ok ? " OK" : " MISMATCH") << "\n";
                    if (!crc_ok && !options.payload.empty()) {
                        payload_failure = true;
                    }
                }

                // Dump decoded payload bytes to file if requested
                if (options.dump_payload && expected_payload_len > 0) {
                    std::ofstream payload_file(*options.dump_payload, std::ios::binary);
                    if (payload_file) {
                        std::size_t bytes_to_write = std::min<std::size_t>(
                            unwhitened.size(), expected_payload_len);
                        payload_file.write(reinterpret_cast<const char*>(unwhitened.data()),
                                           static_cast<std::streamsize>(bytes_to_write));
                        std::cout << "[dump-payload] wrote " << bytes_to_write
                                  << " bytes to " << options.dump_payload->string() << "\n";
                    } else {
                        std::cerr << "[dump-payload] failed to open "
                                  << options.dump_payload->string() << "\n";
                    }
                }

                if (options.dump_stages && have_stage_outputs) {
                    std::filesystem::path base = *options.dump_stages;
                    if (base.extension() == ".cf32") {
                        base.replace_extension("");
                    }
                    auto dump_stage_file = [&](const char* suffix,
                                               const auto& host_vec) {
                        std::filesystem::path path = base;
                        path += suffix;
                        write_stage_file(path, host_vec);
                    };
                    dump_stage_file("_fft.txt", stage_outputs.fft);
                    dump_stage_file("_gray.txt", stage_outputs.gray);
                    dump_stage_file("_deinterleaver.txt", stage_outputs.deinterleaver);
                    dump_stage_file("_hamming.txt", stage_outputs.hamming);
                    std::cout << "Dumped stage outputs using prefix " << base.generic_string() << "\n";
                }

                if (options.compare_root && have_stage_outputs) {
                    auto stage_results = compare_with_reference(stage_outputs, *options.compare_root);
                    summary.stage_results = stage_results;
                    summary.compare_run = true;
                    static const bool verbose_compare = (std::getenv("HOST_SIM_VERBOSE_COMPARE") != nullptr);
                    std::size_t stage_total = 0;
                    std::ostringstream summary_line;
                    summary_line << "[compare] summary:";
                    std::ostringstream ci_line;
                    ci_line << "[ci-summary] stages";

                    for (const auto& res : stage_results) {
                        if (verbose_compare) {
                            if (res.reference_missing) {
                                std::cout << "[compare] stage " << res.label << ": reference file missing\n";
                            } else {
                                std::cout << "[compare] stage " << res.label << ": host=" << res.host_count
                                          << " ref=" << res.ref_count << " mismatches=" << res.mismatches;
                                if (res.alignment_offset) {
                                    if (res.alignment_relative_to_reference) {
                                        std::cout << " (aligned at ref offset " << *res.alignment_offset << ")";
                                    } else {
                                        std::cout << " (aligned at host offset " << *res.alignment_offset << ")";
                                    }
                                } else {
                                    std::cout << " (no exact alignment, comparing from start)";
                                }
                                if (res.mismatches == 0) {
                                    std::cout << " (OK)\n";
                                } else if (res.first_diff_index) {
                                    std::cout << " first_diff@" << *res.first_diff_index;
                                    if (res.host_value && res.ref_value) {
                                        std::cout << " host=" << *res.host_value
                                                  << " ref=" << *res.ref_value;
                                    }
                                    std::cout << "\n";
                                } else if (res.host_count > 0 && res.ref_count == 0) {
                                    std::cout << " reference empty\n";
                                } else {
                                    std::cout << " length mismatch\n";
                                }
                            }
                        }

                        stage_total += res.mismatches;
                        if (res.mismatches > 0 || res.reference_missing) {
                            compare_failure = true;
                        }

                        summary_line << ' ' << res.label << '=' << build_stage_summary_token(res);
                        ci_line << ' ' << res.label << '=' << res.mismatches << '/' << res.host_count;
                    }

                    total_stage_mismatches += stage_total;

                    std::cout << summary_line.str() << "\n";
                    if (verbose_compare) {
                        if (stage_total == 0) {
                            std::cout << "[compare] all stage outputs match reference\n";
                        } else {
                            std::cout << "[compare] aggregate stage mismatches: " << stage_total << "\n";
                        }
                    } else if (stage_total > 0) {
                        std::cout << "[compare] stage mismatches detected: " << stage_total << "\n";
                    }
                    std::cout << ci_line.str() << "\n";
                }
                if (options.summary_output) {
                    auto instrumentation = run_scheduler_instrumentation(
                        samples,
                        *metadata,
                        alignment_samples,
                        256);
                    summary.stage_timings_ns = std::move(instrumentation.stage_timings_ns);
                    summary.memory_usage_bytes = std::move(instrumentation.symbol_memory_bytes);
                }
            } // end if (header.success)

            // --- multi-packet loop advance ---
            if (options.multi_packet) {
                // Estimate burst end: data start + consumed symbols.
                // data_start_sample is updated by successful re-demod
                // to track alignment + sync_pos + quarter offset.
                const std::size_t symbols_consumed =
                    header.success ? symbol_cursor : symbols.size();
                const std::size_t effective_start =
                    header.success ? data_start_sample : alignment_samples;
                const std::size_t burst_end_sample = effective_start +
                    symbols_consumed * static_cast<std::size_t>(sps);
                // Advance past this burst with a small gap margin
                const std::size_t next_offset = burst_end_sample + static_cast<std::size_t>(sps) * 4;
                // If we didn't advance (e.g., no symbols decoded), skip one symbol period
                if (next_offset <= multi_search_offset) {
                    multi_search_offset += static_cast<std::size_t>(sps) * 8;
                } else {
                    multi_search_offset = next_offset;
                }
                if (!header.success) {
                    std::cout << "[multi] packet #" << multi_packet_index
                              << ": header decode failed, skipping burst\n";
                }
                ++multi_packet_index;
                symbols.clear();
                continue; // next burst
            }
            break; // single-packet mode: done
          } // end multi-packet loop
            summary.stage_mismatches = total_stage_mismatches;
        }

        if (options.dump_symbols && !symbols.empty()) {
            std::ofstream sym_out(*options.dump_symbols);
            if (!sym_out) {
                throw std::runtime_error("Failed to open symbol dump file: " + options.dump_symbols->string());
            }
            for (std::size_t i = 0; i < symbols.size(); ++i) {
                sym_out << symbols[i] << '\n';
            }
            std::cout << "Dumped " << symbols.size() << " symbols to " << options.dump_symbols->generic_string() << "\n";
        }

        if (options.dump_iq) {
            std::ofstream iq_out(*options.dump_iq, std::ios::binary);
            if (!iq_out) {
                throw std::runtime_error("Failed to open IQ dump file: " + options.dump_iq->string());
            }
            const float* raw = reinterpret_cast<const float*>(&samples[alignment_samples]);
            const std::size_t float_count = (samples.size() > alignment_samples ? samples.size() - alignment_samples : 0) * 2;
            iq_out.write(reinterpret_cast<const char*>(raw), static_cast<std::streamsize>(float_count * sizeof(float)));
            std::cout << "Dumped aligned IQ to " << options.dump_iq->generic_string() << "\n";
        }
        std::vector<uint8_t> payload_bytes(options.payload.begin(), options.payload.end());
        host_sim::WhiteningSequencer sequencer;
        const auto whitened = sequencer.apply(payload_bytes);
        const auto recovered = sequencer.undo(whitened);

        const bool roundtrip_ok = (recovered == payload_bytes);

        std::cout << "Whitening preview: ";
        for (std::size_t i = 0; i < std::min<std::size_t>(whitened.size(), 8); ++i) {
            std::cout << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                      << static_cast<int>(whitened[i]) << ' ';
        }
        std::cout << std::dec << "\nRound-trip whitening " << (roundtrip_ok ? "succeeded" : "FAILED") << "\n";

        if (options.stats_output) {
            write_stats_json(*options.stats_output, stats, options);
            std::cout << "Wrote stats to " << options.stats_output->generic_string() << "\n";
        }

        summary.whitening_roundtrip_ok = roundtrip_ok;
        summary.stage_mismatches = total_stage_mismatches;
        summary.reference_mismatches = reference_mismatches;
        if (options.summary_output) {
            write_summary_json(*options.summary_output, summary);
            std::cout << "Wrote summary to " << options.summary_output->generic_string() << "\n";
        }

        if (reference_mismatches > 0) {
            compare_failure = true;
            std::cout << "[summary] reference demod mismatches observed: " << reference_mismatches << "\n";
        }
        if (total_stage_mismatches > 0) {
            compare_failure = true;
            std::cout << "[summary] stage comparison mismatches observed: " << total_stage_mismatches << "\n";
        }

        const bool success = roundtrip_ok && !payload_failure &&
                             (!compare_failure || !options.compare_root);
        return success ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
