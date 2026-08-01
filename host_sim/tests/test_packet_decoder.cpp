/// Tests for the packet-level decode_packet() API.
///
/// These run against the real RFM95 over-the-air captures, so they only make
/// sense when the Git LFS data is present; CMake registers them accordingly.

#include "host_sim/capture.hpp"
#include "host_sim/lora_params.hpp"
#include "host_sim/packet_decoder.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

std::string to_string(const std::vector<uint8_t>& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

/// Decode one capture and require the exact payload with a verified CRC.
void expect_payload(const std::filesystem::path& data_dir, const std::string& capture,
                    const std::string& metadata, const std::string& expected)
{
    const auto iq_path = data_dir / capture;
    const auto meta_path = data_dir / metadata;
    if (!std::filesystem::exists(iq_path) || !std::filesystem::exists(meta_path))
    {
        std::cerr << "FAIL: missing test data " << iq_path << "\n";
        ++failures;
        return;
    }

    const auto samples = host_sim::load_cf32(iq_path);
    const auto meta = host_sim::load_metadata(meta_path);

    const auto packet = host_sim::decode_packet(samples, meta);
    if (!packet.has_value())
    {
        std::cerr << "FAIL: no packet decoded from " << capture << "\n";
        ++failures;
        return;
    }

    check(packet->crc_ok, capture + ": CRC should verify");
    check(to_string(packet->payload) == expected,
          capture + ": payload \"" + to_string(packet->payload) + "\" != \"" + expected + "\"");
    check(packet->payload_len == static_cast<int>(expected.size()),
          capture + ": payload_len should match the decoded payload");
    check(packet->alignment >= packet->burst_offset,
          capture + ": alignment should not precede the burst");
    check(packet->consumed > packet->alignment,
          capture + ": consumed should advance past alignment");
}

} // namespace

int main()
{
    const std::filesystem::path data_dir =
        std::filesystem::path(PROJECT_SOURCE_DIR) / "host_sim" / "data" / "ota";

    // The four live RFM95 packets, captured over the air via HackRF.
    expect_payload(data_dir, "packet_0.cf32", "metadata.json", "LoRa Test #520");
    expect_payload(data_dir, "packet_1.cf32", "metadata.json", "LoRa Test #521");
    expect_payload(data_dir, "packet_2.cf32", "metadata.json", "LoRa Test #522");
    expect_payload(data_dir, "packet_3.cf32", "metadata.json", "LoRa Test #523");

    // A spread of the golden parameter sweep: several spreading factors,
    // bandwidths and coding rates through the same one-call entry point.
    expect_payload(data_dir, "sf7_bw125_cr5.cf32", "sf7_bw125_cr5_meta.json", "LoRa Test #12");
    expect_payload(data_dir, "sf7_bw250_cr5.cf32", "sf7_bw250_cr5_meta.json", "LoRa Test #2");
    expect_payload(data_dir, "sf8_bw125_cr5.cf32", "sf8_bw125_cr5_meta.json", "LoRa Test #4");
    expect_payload(data_dir, "sf9_bw125_cr5.cf32", "sf9_bw125_cr5_meta.json", "LoRa Test #5");
    expect_payload(data_dir, "sf10_bw125_cr5.cf32", "sf10_bw125_cr5_meta.json", "LoRa Test #74");
    expect_payload(data_dir, "sf11_bw125_cr5.cf32", "sf11_bw125_cr5_meta.json", "LoRa Test #77");
    expect_payload(data_dir, "sf12_bw125_cr5.cf32", "sf12_bw125_cr5_meta.json", "LoRa Test #79");

    // A buffer of noise must not produce a packet claiming a verified CRC.
    {
        std::vector<std::complex<float>> noise(200000, {0.001f, -0.001f});
        host_sim::LoRaMetadata meta{};
        meta.sf = 7;
        meta.bw = 125000;
        meta.sample_rate = 500000;
        meta.cr = 1;
        meta.preamble_len = 8;
        meta.has_crc = true;
        meta.sync_word = 0x12;
        const auto packet = host_sim::decode_packet(noise, meta);
        check(!packet.has_value() || !packet->crc_ok, "noise must not yield a CRC-verified packet");
    }

    if (failures == 0)
    {
        std::cout << "test_packet_decoder: all checks passed\n";
        return 0;
    }
    std::cerr << "test_packet_decoder: " << failures << " check(s) failed\n";
    return 1;
}
