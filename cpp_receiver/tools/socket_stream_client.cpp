#include "iq_loader.hpp"
#include "streaming_receiver.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class SimpleJson {
public:
    explicit SimpleJson(const std::filesystem::path &path) {
        std::ifstream ifs(path);
        if (!ifs) {
            throw std::runtime_error("failed to open json file: " + path.string());
        }
        text_.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    template <typename T>
    std::optional<T> get(std::string_view key) const {
        const std::string token = '"' + std::string(key) + '"';
        auto pos = text_.find(token);
        if (pos == std::string::npos) { return std::nullopt; }
        pos = text_.find(':', pos);
        if (pos == std::string::npos) { return std::nullopt; }
        ++pos;
        while (pos < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos]))) { ++pos; }
        if (pos >= text_.size()) { return std::nullopt; }

        if constexpr (std::is_same_v<T, bool>) {
            if (text_.compare(pos, 4, "true") == 0) { return true; }
            if (text_.compare(pos, 5, "false") == 0) { return false; }
            return std::nullopt;
        } else if constexpr (std::is_integral_v<T>) {
            std::size_t end = pos;
            while (end < text_.size() &&
                   (std::isdigit(static_cast<unsigned char>(text_[end])) || text_[end] == '-' ||
                    text_[end] == 'x' || text_[end] == 'X')) {
                ++end;
            }
            std::string value_str = text_.substr(pos, end - pos);
            try {
                if constexpr (std::is_unsigned_v<T>) {
                    return static_cast<T>(std::stoul(value_str, nullptr, 0));
                } else {
                    return static_cast<T>(std::stol(value_str, nullptr, 0));
                }
            } catch (...) {
                return std::nullopt;
            }
        } else if constexpr (std::is_floating_point_v<T>) {
            std::size_t end = pos;
            while (end < text_.size() &&
                   (std::isdigit(static_cast<unsigned char>(text_[end])) || text_[end] == '.' ||
                    text_[end] == '-' || text_[end] == '+' || text_[end] == 'e' || text_[end] == 'E')) {
                ++end;
            }
            std::string value_str = text_.substr(pos, end - pos);
            try {
                return static_cast<T>(std::stod(value_str));
            } catch (...) {
                return std::nullopt;
            }
        } else {
            if (text_[pos] != '"') { return std::nullopt; }
            ++pos;
            auto end = text_.find('"', pos);
            if (end == std::string::npos) { return std::nullopt; }
            return text_.substr(pos, end - pos);
        }
    }

private:
    std::string text_;
};

constexpr int clamp_ldro_mode(int mode) {
    switch (mode) {
    case 1:
        return 1;
    case 2:
        return 2;
    default:
        return 0;
    }
}

constexpr lora::DecodeParams::LdroMode ldro_mode_from_int(int mode) {
    switch (mode) {
    case 1:
        return lora::DecodeParams::LdroMode::On;
    case 2:
        return lora::DecodeParams::LdroMode::Auto;
    default:
        return lora::DecodeParams::LdroMode::Off;
    }
}

struct FrameMeta {
    std::filesystem::path json_path;
    int sf = 7;
    int bw = 125000;
    int fs = 500000;
    int cr = 4;
    int ldro_mode = 2;
    bool implicit = false;
    bool crc = true;
    unsigned sync = 0x12u;
    std::string name;
    std::optional<double> sampling_offset_ppm;
};

struct Args {
    std::string host = "127.0.0.1";
    int port = 9000;
    bool emit_bytes = false;
    std::vector<std::filesystem::path> metas;
    std::optional<std::string> unix_path;
    std::optional<double> ppm_offset;
};

void print_usage(const char *prog) {
    std::cout << "Usage: " << prog << " [options] <meta1.json> <meta2.json> ...\n"
              << "Options:\n"
              << "  --host <addr>        TCP host to connect (default 127.0.0.1)\n"
              << "  --port <int>         TCP port (default 9000)\n"
              << "  --unix <path>        Unix domain socket path (overrides host/port)\n"
              << "  --emit-bytes         Emit payload byte events\n"
              << "  --ppm-offset <ppm>   Override sampling offset (ppm) for all frames\n";
}

Args parse_args(int argc, char **argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string cur = argv[i];
        if (cur == "--host" && i + 1 < argc) {
            args.host = argv[++i];
        } else if (cur == "--port" && i + 1 < argc) {
            args.port = std::stoi(argv[++i]);
        } else if (cur == "--emit-bytes") {
            args.emit_bytes = true;
        } else if (cur == "--unix" && i + 1 < argc) {
            args.unix_path = argv[++i];
        } else if (cur == "--ppm-offset" && i + 1 < argc) {
            args.ppm_offset = std::stod(argv[++i]);
        } else if (cur == "--help" || cur == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (!cur.empty() && cur.front() == '-') {
            throw std::runtime_error("unknown option: " + cur);
        } else {
            args.metas.emplace_back(cur);
        }
    }
    if (args.metas.empty()) {
        throw std::runtime_error("at least one metadata JSON path is required");
    }
    return args;
}

FrameMeta load_metadata(const std::filesystem::path &json_path) {
    SimpleJson json(json_path);
    FrameMeta meta;
    meta.json_path = json_path;
    meta.sf = json.get<int>("sf").value_or(7);
    meta.bw = json.get<int>("bw").value_or(125000);
    meta.fs = json.get<int>("samp_rate").value_or(json.get<int>("sample_rate").value_or(500000));
    meta.cr = json.get<int>("cr").value_or(4);
    if (auto mode_int = json.get<int>("ldro_mode")) {
        meta.ldro_mode = clamp_ldro_mode(*mode_int);
    } else if (auto mode_bool = json.get<bool>("ldro_mode")) {
        meta.ldro_mode = *mode_bool ? 1 : 0;
    } else if (auto ldro_int = json.get<int>("ldro")) {
        meta.ldro_mode = clamp_ldro_mode(*ldro_int);
    } else if (auto ldro_bool = json.get<bool>("ldro")) {
        meta.ldro_mode = *ldro_bool ? 1 : 0;
    }
    meta.implicit = json.get<bool>("implicit_header").value_or(false);
    meta.crc = json.get<bool>("crc").value_or(true);
    meta.sync = json.get<unsigned>("sync_word").value_or(0x12u);
    if (auto name = json.get<std::string>("filename")) {
        meta.name = *name;
    } else {
        meta.name = json_path.stem().string();
    }
    if (auto ppm = json.get<double>("sampling_offset_ppm")) {
        meta.sampling_offset_ppm = *ppm;
    } else if (auto ppm_alt = json.get<double>("ppm_offset")) {
        meta.sampling_offset_ppm = *ppm_alt;
    }
    return meta;
}

bool read_exact(int fd, void *buf, std::size_t len) {
    std::size_t done = 0;
    auto *ptr = static_cast<std::uint8_t *>(buf);
    while (done < len) {
        const ssize_t ret = ::recv(fd, ptr + done, len - done, 0);
        if (ret <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(ret);
    }
    return true;
}

struct FrameSummary {
    std::string name;
    bool success = false;
    std::size_t payload_len = 0;
    std::size_t bytes_emitted = 0;
    bool has_sample_rate_hint = false;
    double sample_rate_hint_ratio = 1.0;
    bool has_sample_rate_result = false;
    double sample_rate_ratio_used = 1.0;
    int sr_scan_attempts = 0;
    int sr_scan_successes = 0;
    int cfo_sweep_attempts = 0;
    int cfo_sweep_successes = 0;
    int payload_retry_attempts = 0;
    bool used_cached_sample_rate = false;
    double sync_time_us = 0.0;
    double header_time_us = 0.0;
    double payload_time_us = 0.0;
    double resample_time_us = 0.0;
    double retry_time_us = 0.0;
};

struct ReceiverContext {
    lora::StreamingReceiver receiver;
    FrameMeta meta;
    FrameSummary summary;
    bool done = false;
    bool error = false;
    bool sr_retry_attempted = false;

    ReceiverContext(const FrameMeta &info, bool emit_bytes)
        : receiver(prepare_params(info, emit_bytes)), meta(info) {
        summary.name = info.name;
        if (info.sampling_offset_ppm.has_value()) {
            summary.has_sample_rate_hint = true;
            summary.sample_rate_hint_ratio = 1.0 + (*info.sampling_offset_ppm) * 1e-6;
        }
    }

    void process_events(const std::vector<lora::StreamingReceiver::FrameEvent> &events) {
        for (const auto &ev : events) {
            switch (ev.type) {
            case lora::StreamingReceiver::FrameEvent::Type::PayloadByte:
                if (ev.payload_byte.has_value()) {
                    ++summary.bytes_emitted;
                }
                break;
            case lora::StreamingReceiver::FrameEvent::Type::FrameDone: {
                const bool success = ev.result ? ev.result->success : false;
                summary.payload_len = ev.result ? ev.result->payload.size() : 0;
                if (ev.result) {
                    summary.has_sample_rate_result = true;
                    summary.sample_rate_ratio_used = ev.result->sample_rate_ratio_used;
                    summary.sr_scan_attempts = ev.result->sr_scan_attempts;
                    summary.sr_scan_successes = ev.result->sr_scan_successes;
                    summary.cfo_sweep_attempts = ev.result->cfo_sweep_attempts;
                    summary.cfo_sweep_successes = ev.result->cfo_sweep_successes;
                    summary.payload_retry_attempts = ev.result->payload_retry_attempts;
                    summary.used_cached_sample_rate = ev.result->used_cached_sample_rate;
                    summary.sync_time_us = ev.result->sync_time_us;
                    summary.header_time_us = ev.result->header_time_us;
                    summary.payload_time_us = ev.result->payload_time_us;
                    summary.resample_time_us = ev.result->resample_time_us;
                    summary.retry_time_us = ev.result->retry_time_us;
                    std::cout << "[socket_debug] frame_done success=" << (ev.result->success ? "yes" : "no")
                              << " payload_crc_ok=" << (ev.result->payload_crc_ok ? "yes" : "no")
                              << " sr_ratio_used=" << std::fixed << std::setprecision(9)
                              << ev.result->sample_rate_ratio_used
                              << " sr_scan=" << ev.result->sr_scan_successes << "/" << ev.result->sr_scan_attempts
                              << " cfo_sweep=" << ev.result->cfo_sweep_successes << "/" << ev.result->cfo_sweep_attempts
                              << " payload_retries=" << ev.result->payload_retry_attempts
                              << " cached_sr=" << (ev.result->used_cached_sample_rate ? "yes" : "no")
                              << " t_sync_us=" << ev.result->sync_time_us
                              << " t_header_us=" << ev.result->header_time_us
                              << " t_payload_us=" << ev.result->payload_time_us
                              << " t_resample_us=" << ev.result->resample_time_us
                              << " t_retry_us=" << ev.result->retry_time_us
                              << std::endl;
                    std::cout.unsetf(std::ios::floatfield);
                }
                if (success) {
                    done = true;
                    summary.success = true;
                    break;
                }
                if (!sr_retry_attempted) {
                    sr_retry_attempted = true;
                    const std::size_t sps = std::max<std::size_t>(1, static_cast<std::size_t>(meta.fs / meta.bw));
                    const std::size_t flush_symbols = 64;
                    const std::size_t flush_samples = sps * flush_symbols;
                    std::vector<lora::StreamingReceiver::Sample> zeros(flush_samples, {0.0f, 0.0f});
                    auto retry_events = receiver.push_samples(zeros);
                    process_events(retry_events);
                    if (summary.success || error) {
                        done = true;
                    } else {
                        done = true;
                        summary.success = false;
                    }
                } else {
                    done = true;
                    summary.success = false;
                }
                break;
            }
            case lora::StreamingReceiver::FrameEvent::Type::FrameError:
                done = true;
                error = true;
                summary.success = false;
                break;
            default:
                break;
            }
            if (done) {
                break;
            }
        }
    }

    static lora::DecodeParams prepare_params(const FrameMeta &meta, bool emit_bytes) {
        lora::DecodeParams params;
        params.sf = meta.sf;
        params.bandwidth_hz = meta.bw;
        params.sample_rate_hz = meta.fs;
        params.ldro_mode = ldro_mode_from_int(meta.ldro_mode);
        params.sync_word = meta.sync;
        params.skip_sync_word_check = false;
        params.implicit_header = meta.implicit;
        params.implicit_payload_length = 0;
        params.implicit_has_crc = meta.crc;
        params.implicit_cr = meta.cr;
        params.emit_payload_bytes = emit_bytes;
        if (meta.sampling_offset_ppm.has_value()) {
            params.sample_rate_ratio = 1.0 + (*meta.sampling_offset_ppm) * 1e-6;
        }
        return params;
    }

    void process_chunk(std::span<const lora::StreamingReceiver::Sample> span) {
        if (done) {
            return;
        }
        auto events = receiver.push_samples(span);
        process_events(events);
    }
};

} // namespace

int main(int argc, char **argv) {
    try {
        Args args = parse_args(argc, argv);

        std::vector<FrameMeta> metas;
        metas.reserve(args.metas.size());
        for (const auto &path : args.metas) {
            FrameMeta meta = load_metadata(path);
            if (args.ppm_offset.has_value()) {
                meta.sampling_offset_ppm = args.ppm_offset;
            }
            metas.push_back(std::move(meta));
        }

        int sock = -1;
        if (args.unix_path.has_value()) {
            sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (sock < 0) {
                throw std::runtime_error("socket() failed");
            }
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            if (args.unix_path->size() >= sizeof(addr.sun_path)) {
                ::close(sock);
                throw std::runtime_error("unix socket path too long");
            }
            std::strncpy(addr.sun_path, args.unix_path->c_str(), sizeof(addr.sun_path) - 1);
            if (::connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
                ::close(sock);
                throw std::runtime_error("connect() failed (unix)");
            }
        } else {
            sock = ::socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                throw std::runtime_error("socket() failed");
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(args.port));
            if (::inet_pton(AF_INET, args.host.c_str(), &addr.sin_addr) != 1) {
                ::close(sock);
                throw std::runtime_error("inet_pton failed for host " + args.host);
            }

            if (::connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
                ::close(sock);
                throw std::runtime_error("connect() failed");
            }
        }

        std::size_t frame_index = 0;
        std::optional<ReceiverContext> ctx;
        auto advance_frame = [&]() {
            if (frame_index >= metas.size()) {
                ctx.reset();
                return;
            }
            ctx.emplace(metas[frame_index], args.emit_bytes);
        };
        advance_frame();

        while (ctx.has_value()) {
            std::uint32_t length_be = 0;
            if (!read_exact(sock, &length_be, sizeof(length_be))) {
                break;
            }
            std::uint32_t sample_count = ntohl(length_be);

            if (sample_count == 0) {
                if (!ctx->done && !ctx->error) {
                    const std::size_t sps = static_cast<std::size_t>(ctx->meta.fs / ctx->meta.bw);
                    const std::size_t flush_symbols = 8;
                    const std::size_t flush_samples = sps * std::max<std::size_t>(2, flush_symbols);
                    std::vector<lora::StreamingReceiver::Sample> zeros(flush_samples, {0.0f, 0.0f});
                    ctx->process_chunk(zeros);
                }

                if (ctx->done || ctx->error) {
                    std::cout << "[socket_frame " << (frame_index + 1) << "] " << ctx->summary.name
                              << " success=" << (ctx->summary.success ? "yes" : "no")
                              << " payload_len=" << ctx->summary.payload_len
                              << " payload_bytes_events=" << ctx->summary.bytes_emitted;
                    if (ctx->summary.has_sample_rate_hint || ctx->summary.has_sample_rate_result) {
                        const double hint = ctx->summary.has_sample_rate_hint ? ctx->summary.sample_rate_hint_ratio : 1.0;
                        const double used = ctx->summary.has_sample_rate_result ? ctx->summary.sample_rate_ratio_used : 1.0;
                        std::cout << " sr_hint=" << std::fixed << std::setprecision(9) << hint
                                  << " sr_used=" << used;
                        std::cout.unsetf(std::ios::floatfield);
                    }
                    std::cout << " sr_scan=" << ctx->summary.sr_scan_successes << "/" << ctx->summary.sr_scan_attempts
                              << " cfo_sweep=" << ctx->summary.cfo_sweep_successes << "/" << ctx->summary.cfo_sweep_attempts
                              << " payload_retries=" << ctx->summary.payload_retry_attempts
                              << " cached_sr=" << (ctx->summary.used_cached_sample_rate ? "yes" : "no")
                              << " t_sync_us=" << ctx->summary.sync_time_us
                              << " t_header_us=" << ctx->summary.header_time_us
                              << " t_payload_us=" << ctx->summary.payload_time_us
                              << " t_resample_us=" << ctx->summary.resample_time_us
                              << " t_retry_us=" << ctx->summary.retry_time_us;
                    std::cout << '\n';
                    ++frame_index;
                    advance_frame();
                }
                continue;
            }

            std::vector<float> raw(static_cast<std::size_t>(sample_count) * 2);
            if (!read_exact(sock, raw.data(), raw.size() * sizeof(float))) {
                break;
            }

            std::vector<lora::StreamingReceiver::Sample> chunk(sample_count);
            for (std::size_t i = 0; i < sample_count; ++i) {
                chunk[i] = {raw[2 * i + 0], raw[2 * i + 1]};
            }
            ctx->process_chunk(chunk);
        }

        ::close(sock);
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "[ERROR] " << ex.what() << '\n';
        return 1;
    }
}
