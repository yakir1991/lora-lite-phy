#include "host_sim/lora_params.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Failed to open metadata JSON: " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string strip_spaces(std::string value)
{
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch); }), value.end());
    return value;
}

std::optional<std::string> find_value(const std::string& content, const std::string& key)
{
    const std::string pattern = "\"" + key + "\"";
    const auto pos = content.find(pattern);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    auto colon = content.find(':', pos + pattern.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    auto start = colon + 1;
    auto end = start;
    bool in_string = false;
    while (end < content.size()) {
        char ch = content[end];
        if (!in_string && (ch == ',' || ch == '\n' || ch == '}')) {
            break;
        }
        if (ch == '"' && (end == start || content[end - 1] != '\\')) {
            if (in_string) {
                ++end;
                break;
            } else {
                in_string = true;
            }
        }
        ++end;
    }
    std::string raw = content.substr(start, end - start);
    return strip_spaces(raw);
}

int parse_int(const std::string& token, int default_value)
{
    try {
        size_t idx = 0;
        int value = std::stoi(token, &idx, 0);
        (void)idx;
        return value;
    } catch (...) {
        return default_value;
    }
}

bool parse_bool(const std::string& token, bool default_value)
{
    if (token == "true" || token == "True") {
        return true;
    }
    if (token == "false" || token == "False") {
        return false;
    }
    return default_value;
}

std::optional<std::string> parse_string(const std::string& token)
{
    if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
        return token.substr(1, token.size() - 2);
    }
    return std::nullopt;
}

} // namespace

namespace host_sim
{

LoRaMetadata load_metadata(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Metadata JSON not found: " + path.string());
    }

    const auto content = read_file(path);
    LoRaMetadata meta;

    if (auto value = find_value(content, "sf")) {
        meta.sf = parse_int(*value, meta.sf);
    }
    if (auto value = find_value(content, "bw")) {
        meta.bw = parse_int(*value, meta.bw);
    }
    if (auto value = find_value(content, "sample_rate")) {
        meta.sample_rate = parse_int(*value, meta.sample_rate);
    } else {
        meta.sample_rate = meta.bw * 4;
    }
    if (auto value = find_value(content, "cr")) {
        meta.cr = parse_int(*value, meta.cr);
    }
    if (auto value = find_value(content, "payload_len")) {
        meta.payload_len = parse_int(*value, meta.payload_len);
    }
    if (auto value = find_value(content, "preamble_len")) {
        meta.preamble_len = parse_int(*value, meta.preamble_len);
    }
    if (auto value = find_value(content, "has_crc")) {
        meta.has_crc = parse_bool(*value, meta.has_crc);
    }
    if (auto value = find_value(content, "implicit_header")) {
        meta.implicit_header = parse_bool(*value, meta.implicit_header);
    }
    if (auto value = find_value(content, "ldro")) {
        meta.ldro = parse_bool(*value, meta.ldro);
    }
    if (auto value = find_value(content, "sync_word")) {
        meta.sync_word = parse_int(*value, meta.sync_word);
    }
    if (auto value = find_value(content, "payload_hex")) {
        meta.payload_hex = parse_string(*value);
    }

    return meta;
}

} // namespace host_sim
