#include "host_sim/impairments.hpp"
#include "host_sim/live_source.hpp"
#include "host_sim/scheduler.hpp"
#include "host_sim/stage.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

using MicroDuration = std::chrono::duration<double, std::micro>;

struct RunOptions
{
    enum class Mode
    {
        Synthetic,
        Capture
    };

    Mode mode{Mode::Synthetic};
    std::filesystem::path capture_path;
    std::filesystem::path metadata_path;
    std::filesystem::path metrics_path{"stage5_live_soak_metrics.json"};

    host_sim::StageConfig stage_config{9, 125000, 500000};
    std::size_t synthetic_symbols{4096};
    double symbol_interval_us{200.0};
    std::size_t samples_per_symbol{64};
    bool samples_per_symbol_overridden{false};
    std::optional<std::size_t> max_symbols;

    host_sim::ImpairmentConfig impairment{};
};

struct CaptureMetadata
{
    host_sim::StageConfig config{};
    std::size_t samples_per_symbol{0};
    double symbol_interval_us{0.0};
};

struct SoakMetrics
{
    std::string mode{"synthetic"};
    std::size_t expected_symbols{0};
    std::size_t processed_symbols{0};
    std::size_t produced_symbols{0};
    std::size_t producer_failures{0};
    std::size_t missing_symbols{0};
    double first_failure_time_us{-1.0};
    double duration_us{0.0};
    double mtbf_us{-1.0};
    std::size_t mtbf_symbols{0};
    double symbol_interval_us{0.0};
    std::size_t samples_per_symbol{0};
    host_sim::StageConfig stage_config{};
    std::filesystem::path capture_file;
    std::filesystem::path metadata_file;
    bool impairments_enabled{false};
    uint32_t impairment_seed{0};
};

class MetricsStage : public host_sim::Stage
{
public:
    explicit MetricsStage(SoakMetrics& metrics)
        : metrics_(metrics)
    {
    }

    void reset(const host_sim::StageConfig&) override
    {
        metrics_.processed_symbols = 0;
    }

    void process(host_sim::SymbolContext&) override
    {
        ++metrics_.processed_symbols;
    }

    void flush() override {}

private:
    SoakMetrics& metrics_;
};

std::string json_escape(std::string_view input)
{
    std::string escaped;
    escaped.reserve(input.size());
    for (char ch : input) {
        if (ch == '"' || ch == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::optional<double> find_number_field(const std::string& payload, std::string_view key)
{
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto pos = payload.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    auto colon = payload.find(':', pos + needle.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    ++colon;
    while (colon < payload.size() && std::isspace(static_cast<unsigned char>(payload[colon]))) {
        ++colon;
    }
    std::size_t end = colon;
    while (end < payload.size()) {
        const char c = payload[end];
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' ||
              c == '.' || c == 'e' || c == 'E')) {
            break;
        }
        ++end;
    }
    if (end == colon) {
        return std::nullopt;
    }
    try {
        return std::stod(payload.substr(colon, end - colon));
    } catch (...) {
        return std::nullopt;
    }
}

template <typename T>
T read_number_or_throw(const std::string& payload, std::string_view key)
{
    const auto value = find_number_field(payload, key);
    if (!value) {
        throw std::runtime_error("Missing field '" + std::string(key) + "' in metadata");
    }
    return static_cast<T>(*value);
}

CaptureMetadata load_capture_metadata(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to open metadata file: " + path.string());
    }
    std::string payload((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    CaptureMetadata metadata;
    metadata.config.sf = read_number_or_throw<int>(payload, "sf");
    metadata.config.bandwidth = read_number_or_throw<int>(payload, "bw_hz");
    metadata.config.sample_rate = read_number_or_throw<int>(payload, "sample_rate_hz");

    double samples_per_symbol =
        find_number_field(payload, "samples_per_symbol").value_or(0.0);
    if (samples_per_symbol <= 0.0) {
        const double chips = std::pow(2.0, static_cast<double>(metadata.config.sf));
        const double bw = static_cast<double>(metadata.config.bandwidth);
        const double sample_rate = static_cast<double>(metadata.config.sample_rate);
        if (chips <= 0.0 || bw <= 0.0 || sample_rate <= 0.0) {
            throw std::runtime_error("Invalid metadata parameters for samples_per_symbol");
        }
        const double symbol_period_s = chips / bw;
        samples_per_symbol = symbol_period_s * sample_rate;
    }
    metadata.samples_per_symbol =
        static_cast<std::size_t>(std::max(1.0, std::round(samples_per_symbol)));
    const double sample_rate = static_cast<double>(metadata.config.sample_rate);
    if (sample_rate <= 0.0) {
        throw std::runtime_error("sample_rate_hz must be positive in metadata");
    }
    metadata.symbol_interval_us =
        (static_cast<double>(metadata.samples_per_symbol) / sample_rate) * 1e6;
    return metadata;
}

std::vector<std::complex<float>> load_cf32(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open capture: " + path.string());
    }
    file.seekg(0, std::ios::end);
    const auto bytes = file.tellg();
    file.seekg(0, std::ios::beg);
    if (bytes <= 0) {
        throw std::runtime_error("Capture is empty: " + path.string());
    }
    if (bytes % static_cast<std::streampos>(sizeof(std::complex<float>)) != 0) {
        throw std::runtime_error("Capture size is not aligned to complex<float>: " + path.string());
    }
    const std::size_t samples =
        static_cast<std::size_t>(bytes / static_cast<std::streampos>(sizeof(std::complex<float>)));
    std::vector<std::complex<float>> data(samples);
    file.read(reinterpret_cast<char*>(data.data()),
              static_cast<std::streamsize>(samples * sizeof(std::complex<float>)));
    if (!file) {
        throw std::runtime_error("Failed to read capture: " + path.string());
    }
    return data;
}

host_sim::SymbolBuffer make_synthetic_symbol(std::size_t symbol_index,
                                             std::size_t samples_per_symbol)
{
    host_sim::SymbolBuffer buffer;
    buffer.samples.resize(samples_per_symbol);
    for (std::size_t i = 0; i < samples_per_symbol; ++i) {
        const float base = static_cast<float>((symbol_index % 32) * 0.03125);
        buffer.samples[i] = std::complex<float>(
            base + static_cast<float>(i) * 0.01F,
            static_cast<float>(i) * 0.001F);
    }
    return buffer;
}

bool push_with_metrics(host_sim::LiveSymbolSource& sink,
                       host_sim::SymbolBuffer symbol,
                       SoakMetrics& metrics,
                       const std::chrono::steady_clock::time_point& start_time)
{
    if (!sink.push_symbol(std::move(symbol), std::chrono::milliseconds(100))) {
        const auto now = std::chrono::steady_clock::now();
        metrics.producer_failures++;
        if (metrics.first_failure_time_us < 0.0) {
            metrics.first_failure_time_us =
                std::chrono::duration<double, std::micro>(now - start_time).count();
        }
        return false;
    }
    ++metrics.produced_symbols;
    return true;
}

void apply_impairment(host_sim::ImpairmentEngine* engine,
                      std::vector<std::complex<float>>& samples)
{
    if (engine != nullptr) {
        engine->apply_capture(samples);
    }
}

bool run_synthetic_producer(host_sim::LiveSymbolSource& source,
                            host_sim::ImpairmentEngine* impairment,
                            SoakMetrics& metrics,
                            std::size_t total_symbols,
                            std::size_t samples_per_symbol,
                            MicroDuration pacing)
{
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < total_symbols; ++i) {
        auto symbol = make_synthetic_symbol(i, samples_per_symbol);
        apply_impairment(impairment, symbol.samples);

        if (!push_with_metrics(source, std::move(symbol), metrics, start)) {
            source.close();
            return false;
        }
        std::this_thread::sleep_for(pacing);
    }
    source.close();
    return true;
}

bool run_capture_producer(host_sim::LiveSymbolSource& source,
                          host_sim::ImpairmentEngine* impairment,
                          SoakMetrics& metrics,
                          const std::vector<std::complex<float>>& capture,
                          std::size_t samples_per_symbol,
                          std::size_t total_symbols,
                          MicroDuration pacing)
{
    if (samples_per_symbol == 0) {
        return false;
    }
    const std::size_t needed_samples = total_symbols * samples_per_symbol;
    if (needed_samples > capture.size()) {
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t symbol = 0; symbol < total_symbols; ++symbol) {
        const std::size_t offset = symbol * samples_per_symbol;
        host_sim::SymbolBuffer buffer;
        buffer.samples.assign(capture.begin() + static_cast<std::ptrdiff_t>(offset),
                              capture.begin() + static_cast<std::ptrdiff_t>(offset + samples_per_symbol));
        apply_impairment(impairment, buffer.samples);

        if (!push_with_metrics(source, std::move(buffer), metrics, start)) {
            source.close();
            return false;
        }
        std::this_thread::sleep_for(pacing);
    }
    source.close();
    return true;
}

std::unique_ptr<host_sim::ImpairmentEngine> build_impairment_engine(
    const host_sim::ImpairmentConfig& config,
    double sample_rate_hz,
    std::size_t samples_per_symbol)
{
    if (!config.enabled()) {
        return nullptr;
    }
    if (sample_rate_hz <= 0.0) {
        throw std::runtime_error("Sample rate must be positive for impairment engine");
    }
    const double symbol_period = static_cast<double>(samples_per_symbol) / sample_rate_hz;
    return std::make_unique<host_sim::ImpairmentEngine>(config,
                                                        sample_rate_hz,
                                                        symbol_period,
                                                        samples_per_symbol);
}

std::size_t parse_size(const std::string& value, const std::string& flag)
{
    try {
        const auto parsed = std::stoll(value);
        if (parsed < 0) {
            throw std::runtime_error("Negative value for " + flag);
        }
        return static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid numeric value for " + flag + ": " + value);
    }
}

double parse_double(const std::string& value, const std::string& flag)
{
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid floating value for " + flag + ": " + value);
    }
}

int parse_int(const std::string& value, const std::string& flag)
{
    try {
        return std::stoi(value);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid integer for " + flag + ": " + value);
    }
}

void print_usage()
{
    std::cout << "host_sim_live_soak usage:\n"
              << "  --mode [synthetic|capture]\n"
              << "  --capture <path>            (capture mode)\n"
              << "  --metadata <path>           (capture mode metadata JSON)\n"
              << "  --json-out <path>           (metrics output, default stage5_live_soak_metrics.json)\n"
              << "  --total-symbols <n>         (synthetic mode)\n"
              << "  --samples-per-symbol <n>    (synthetic mode manual override)\n"
              << "  --symbol-interval-us <val>  (synthetic pacing hint)\n"
              << "  --max-symbols <n>           (limit processed symbols)\n"
              << "  --sf/--bandwidth/--sample-rate (synthetic scheduler config override)\n"
              << "  --impair-* knobs: --impair-cfo-ppm, --impair-sfo-ppm,\n"
              << "     --impair-awgn-snr, --impair-burst-period, --impair-burst-duration,\n"
              << "     --impair-burst-snr, --impair-collision-prob, --impair-collision-scale,\n"
              << "     --impair-collision-waveform, --impair-seed\n";
}

RunOptions parse_options(int argc, char** argv)
{
    RunOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for " + flag);
            }
            return argv[++i];
        };

        if (arg == "--mode") {
            const auto value = require_value("--mode");
            if (value == "synthetic") {
                options.mode = RunOptions::Mode::Synthetic;
            } else if (value == "capture") {
                options.mode = RunOptions::Mode::Capture;
            } else {
                throw std::runtime_error("Unknown mode: " + value);
            }
        } else if (arg == "--capture") {
            options.capture_path = require_value("--capture");
        } else if (arg == "--metadata") {
            options.metadata_path = require_value("--metadata");
        } else if (arg == "--json-out") {
            options.metrics_path = require_value("--json-out");
        } else if (arg == "--total-symbols") {
            options.synthetic_symbols = parse_size(require_value("--total-symbols"),
                                                   "--total-symbols");
        } else if (arg == "--symbol-interval-us") {
            options.symbol_interval_us =
                parse_double(require_value("--symbol-interval-us"), "--symbol-interval-us");
        } else if (arg == "--samples-per-symbol") {
            options.samples_per_symbol =
                parse_size(require_value("--samples-per-symbol"), "--samples-per-symbol");
            options.samples_per_symbol_overridden = true;
        } else if (arg == "--max-symbols") {
            options.max_symbols = parse_size(require_value("--max-symbols"), "--max-symbols");
        } else if (arg == "--sf") {
            options.stage_config.sf = parse_int(require_value("--sf"), "--sf");
        } else if (arg == "--bandwidth") {
            options.stage_config.bandwidth = parse_int(require_value("--bandwidth"), "--bandwidth");
        } else if (arg == "--sample-rate") {
            options.stage_config.sample_rate = parse_int(require_value("--sample-rate"),
                                                         "--sample-rate");
        } else if (arg == "--impair-cfo-ppm") {
            options.impairment.cfo_ppm =
                parse_double(require_value("--impair-cfo-ppm"), "--impair-cfo-ppm");
        } else if (arg == "--impair-cfo-drift-ppm") {
            options.impairment.cfo_drift_ppm_per_s =
                parse_double(require_value("--impair-cfo-drift-ppm"), "--impair-cfo-drift-ppm");
        } else if (arg == "--impair-sfo-ppm") {
            options.impairment.sfo_ppm =
                parse_double(require_value("--impair-sfo-ppm"), "--impair-sfo-ppm");
        } else if (arg == "--impair-sfo-drift-ppm") {
            options.impairment.sfo_drift_ppm_per_s =
                parse_double(require_value("--impair-sfo-drift-ppm"), "--impair-sfo-drift-ppm");
        } else if (arg == "--impair-awgn-snr") {
            options.impairment.awgn_snr_db =
                parse_double(require_value("--impair-awgn-snr"), "--impair-awgn-snr");
            options.impairment.awgn_enabled = true;
        } else if (arg == "--impair-burst-period") {
            options.impairment.burst.period_symbols =
                parse_size(require_value("--impair-burst-period"), "--impair-burst-period");
            options.impairment.burst.enabled = true;
        } else if (arg == "--impair-burst-duration") {
            options.impairment.burst.duration_symbols =
                parse_size(require_value("--impair-burst-duration"), "--impair-burst-duration");
            options.impairment.burst.enabled = true;
        } else if (arg == "--impair-burst-snr") {
            options.impairment.burst.snr_db =
                parse_double(require_value("--impair-burst-snr"), "--impair-burst-snr");
            options.impairment.burst.enabled = true;
        } else if (arg == "--impair-collision-prob") {
            options.impairment.collision.probability =
                parse_double(require_value("--impair-collision-prob"), "--impair-collision-prob");
            options.impairment.collision.enabled = true;
        } else if (arg == "--impair-collision-scale") {
            options.impairment.collision.scale =
                parse_double(require_value("--impair-collision-scale"), "--impair-collision-scale");
            options.impairment.collision.enabled = true;
        } else if (arg == "--impair-collision-waveform") {
            options.impairment.collision.waveform_path =
                require_value("--impair-collision-waveform");
            options.impairment.collision.enabled = true;
        } else if (arg == "--impair-seed") {
            options.impairment.seed =
                static_cast<uint32_t>(parse_size(require_value("--impair-seed"), "--impair-seed"));
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    return options;
}

void write_json(const std::filesystem::path& path,
                const SoakMetrics& metrics,
                const host_sim::ImpairmentConfig& impairment)
{
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Failed to open metrics output: " + path.string());
    }

    out << "{\n";
    out << "  \"mode\": \"" << json_escape(metrics.mode) << "\",\n";
    out << "  \"expected_symbols\": " << metrics.expected_symbols << ",\n";
    out << "  \"processed_symbols\": " << metrics.processed_symbols << ",\n";
    out << "  \"produced_symbols\": " << metrics.produced_symbols << ",\n";
    out << "  \"missing_symbols\": " << metrics.missing_symbols << ",\n";
    out << "  \"producer_failures\": " << metrics.producer_failures << ",\n";
    out << "  \"first_failure_time_us\": " << metrics.first_failure_time_us << ",\n";
    out << "  \"mtbf_us\": " << metrics.mtbf_us << ",\n";
    out << "  \"mtbf_symbols\": " << metrics.mtbf_symbols << ",\n";
    out << "  \"duration_us\": " << metrics.duration_us << ",\n";
    out << "  \"symbol_interval_us\": " << metrics.symbol_interval_us << ",\n";
    out << "  \"samples_per_symbol\": " << metrics.samples_per_symbol << ",\n";
    out << "  \"stage_config\": {\n";
    out << "    \"sf\": " << metrics.stage_config.sf << ",\n";
    out << "    \"bandwidth\": " << metrics.stage_config.bandwidth << ",\n";
    out << "    \"sample_rate\": " << metrics.stage_config.sample_rate << "\n";
    out << "  },\n";
    out << "  \"capture\": {\n";
    out << "    \"file\": \"" << json_escape(metrics.capture_file.generic_string()) << "\",\n";
    out << "    \"metadata\": \"" << json_escape(metrics.metadata_file.generic_string()) << "\"\n";
    out << "  },\n";
    out << "  \"impairments\": {\n";
    out << "    \"enabled\": " << (impairment.enabled() ? "true" : "false") << ",\n";
    out << "    \"seed\": " << impairment.seed << ",\n";
    out << "    \"cfo_ppm\": " << impairment.cfo_ppm << ",\n";
    out << "    \"cfo_drift_ppm_per_s\": " << impairment.cfo_drift_ppm_per_s << ",\n";
    out << "    \"sfo_ppm\": " << impairment.sfo_ppm << ",\n";
    out << "    \"sfo_drift_ppm_per_s\": " << impairment.sfo_drift_ppm_per_s << ",\n";
    out << "    \"awgn_enabled\": " << (impairment.awgn_enabled ? "true" : "false") << ",\n";
    out << "    \"awgn_snr_db\": " << impairment.awgn_snr_db << ",\n";
    out << "    \"burst\": {\n";
    out << "      \"enabled\": " << (impairment.burst.enabled ? "true" : "false") << ",\n";
    out << "      \"period_symbols\": " << impairment.burst.period_symbols << ",\n";
    out << "      \"duration_symbols\": " << impairment.burst.duration_symbols << ",\n";
    out << "      \"snr_db\": " << impairment.burst.snr_db << "\n";
    out << "    },\n";
    out << "    \"collision\": {\n";
    out << "      \"enabled\": " << (impairment.collision.enabled ? "true" : "false") << ",\n";
    out << "      \"probability\": " << impairment.collision.probability << ",\n";
    out << "      \"scale\": " << impairment.collision.scale << ",\n";
    out << "      \"waveform_path\": \"" << json_escape(impairment.collision.waveform_path) << "\"\n";
    out << "    }\n";
    out << "  }\n";
    out << "}\n";
}

SoakMetrics initialise_metrics(const RunOptions& options,
                               const host_sim::StageConfig& config,
                               std::size_t expected_symbols,
                               std::size_t samples_per_symbol,
                               double symbol_interval_us)
{
    SoakMetrics metrics;
    metrics.mode = (options.mode == RunOptions::Mode::Synthetic) ? "synthetic" : "capture";
    metrics.expected_symbols = expected_symbols;
    metrics.samples_per_symbol = samples_per_symbol;
    metrics.symbol_interval_us = symbol_interval_us;
    metrics.stage_config = config;
    metrics.impairments_enabled = options.impairment.enabled();
    metrics.impairment_seed = options.impairment.seed;

    if (options.mode == RunOptions::Mode::Capture) {
        metrics.capture_file = options.capture_path;
        metrics.metadata_file = options.metadata_path;
    }

    return metrics;
}

int run_soak(const RunOptions& options)
{
    host_sim::StageConfig stage_config = options.stage_config;
    std::size_t total_symbols = options.synthetic_symbols;
    std::size_t samples_per_symbol = options.samples_per_symbol;
    double symbol_interval_us = options.symbol_interval_us;
    std::vector<std::complex<float>> capture_samples;

    if (options.mode == RunOptions::Mode::Capture) {
        if (options.capture_path.empty() || options.metadata_path.empty()) {
            throw std::runtime_error("Capture mode requires --capture and --metadata");
        }
        if (!std::filesystem::exists(options.capture_path)) {
            throw std::runtime_error("Capture file does not exist: " + options.capture_path.string());
        }
        if (!std::filesystem::exists(options.metadata_path)) {
            throw std::runtime_error("Metadata file does not exist: " + options.metadata_path.string());
        }
        const auto metadata = load_capture_metadata(options.metadata_path);
        stage_config = metadata.config;
        samples_per_symbol = metadata.samples_per_symbol;
        symbol_interval_us = metadata.symbol_interval_us;

        capture_samples = load_cf32(options.capture_path);
        if (capture_samples.empty()) {
            throw std::runtime_error("Capture contains no samples: " + options.capture_path.string());
        }
        const std::size_t available_symbols = capture_samples.size() / samples_per_symbol;
        if (available_symbols == 0) {
            throw std::runtime_error("Capture shorter than one symbol: " + options.capture_path.string());
        }
        if (options.max_symbols) {
            total_symbols = std::min(available_symbols, *options.max_symbols);
        } else {
            total_symbols = available_symbols;
        }
    } else {
        double sample_rate = static_cast<double>(stage_config.sample_rate);
        if (sample_rate <= 0.0) {
            sample_rate = 1.0;
        }
        if (!options.samples_per_symbol_overridden) {
            const double computed =
                (symbol_interval_us > 0.0) ? sample_rate * symbol_interval_us * 1e-6
                                           : static_cast<double>(samples_per_symbol);
            samples_per_symbol =
                static_cast<std::size_t>(std::max(1.0, std::round(computed)));
        }
        symbol_interval_us =
            (static_cast<double>(samples_per_symbol) / sample_rate) * 1e6;
        if (options.max_symbols) {
            total_symbols = std::min(total_symbols, *options.max_symbols);
        }
    }

    auto metrics = initialise_metrics(options, stage_config, total_symbols, samples_per_symbol,
                                      symbol_interval_us);

    host_sim::LiveSymbolSource source(256);
    auto metrics_stage = std::make_shared<MetricsStage>(metrics);

    host_sim::Scheduler scheduler;
    host_sim::Scheduler::Config scheduler_config{
        stage_config.sf,
        stage_config.bandwidth,
        stage_config.sample_rate};
    scheduler.configure(scheduler_config);
    scheduler.attach_stage(metrics_stage);

    const double sample_rate_hz =
        static_cast<double>(stage_config.sample_rate > 0 ? stage_config.sample_rate : 1);
    auto impairment = build_impairment_engine(options.impairment,
                                              sample_rate_hz,
                                              samples_per_symbol);

    std::atomic<bool> producer_ok{true};
    const MicroDuration pacing(symbol_interval_us);
    const auto start_time = std::chrono::steady_clock::now();

    std::thread producer;
    if (options.mode == RunOptions::Mode::Synthetic) {
        producer = std::thread([&]() {
            if (!run_synthetic_producer(source,
                                        impairment.get(),
                                        metrics,
                                        total_symbols,
                                        samples_per_symbol,
                                        pacing)) {
                producer_ok = false;
            }
        });
    } else {
        producer = std::thread([&]() {
            if (!run_capture_producer(source,
                                      impairment.get(),
                                      metrics,
                                      capture_samples,
                                      samples_per_symbol,
                                      total_symbols,
                                      pacing)) {
                producer_ok = false;
            }
        });
    }

    scheduler.run(source);
    producer.join();

    const auto end_time = std::chrono::steady_clock::now();
    metrics.duration_us =
        std::chrono::duration<double, std::micro>(end_time - start_time).count();
    metrics.missing_symbols =
        (metrics.expected_symbols > metrics.processed_symbols)
            ? (metrics.expected_symbols - metrics.processed_symbols)
            : 0;

    if (metrics.first_failure_time_us < 0.0) {
        metrics.first_failure_time_us = metrics.duration_us;
    }

    if (metrics.producer_failures > 0 || metrics.missing_symbols > 0) {
        metrics.mtbf_us = metrics.first_failure_time_us;
        metrics.mtbf_symbols = metrics.processed_symbols;
    } else {
        metrics.mtbf_us = metrics.duration_us;
        metrics.mtbf_symbols = metrics.processed_symbols;
    }

    write_json(options.metrics_path, metrics, options.impairment);

    if (!producer_ok) {
        return 1;
    }
    if (metrics.processed_symbols != total_symbols) {
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parse_options(argc, argv);
        return run_soak(options);
    } catch (const std::exception& ex) {
        std::cerr << "host_sim_live_soak: " << ex.what() << '\n';
        return 1;
    }
}
