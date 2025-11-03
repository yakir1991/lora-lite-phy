#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace host_sim
{

struct LoRaMetadata
{
    int sf{};
    int bw{};
    int sample_rate{};
    int cr{};
    int payload_len{};
    int preamble_len{8};
    bool has_crc{true};
    bool implicit_header{false};
    bool ldro{false};
    int sync_word{0x12};
    std::optional<std::string> payload_hex;
};

LoRaMetadata load_metadata(const std::filesystem::path& path);

} // namespace host_sim
