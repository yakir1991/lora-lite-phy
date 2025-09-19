#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace lora::rx::gr {

// Gray mapping helpers (GNU Radio compatible: MSB-first)
uint32_t gray_encode(uint32_t value);
uint32_t gray_decode(uint32_t value);

enum class CodeRate : uint8_t {
    CR45 = 1,
    CR46 = 2,
    CR47 = 3,
    CR48 = 4
};

struct InterleaverMap {
    uint32_t n_in{};
    uint32_t n_out{};
    std::vector<uint32_t> map;
};

InterleaverMap make_diagonal_interleaver(uint32_t sf, uint32_t cr_plus4);

struct HammingTables {
    std::array<uint8_t, 16> enc_45{};
    std::array<uint8_t, 16> enc_46{};
    std::array<uint8_t, 16> enc_47{};
    std::array<uint8_t, 16> enc_48{};

    std::array<int8_t, 2>  synd_45{};
    std::array<int8_t, 4>  synd_46{};
    std::array<int8_t, 8>  synd_47{};
    std::array<int8_t, 16> synd_48{};
};

HammingTables make_hamming_tables();

std::pair<uint16_t, uint8_t> hamming_encode4(uint8_t nibble, CodeRate cr, const HammingTables& tables);
std::optional<std::pair<uint8_t, bool>> hamming_decode4(uint16_t cw, uint8_t nbits, CodeRate cr, const HammingTables& tables);

class Crc16Ccitt {
public:
    Crc16Ccitt(uint16_t poly = 0x1021, uint16_t init = 0xFFFF, uint16_t xorout = 0x0000,
               bool reflect_in = false, bool reflect_out = false);

    uint16_t compute(const uint8_t* data, size_t len) const;
    uint16_t compute(const std::vector<uint8_t>& data) const {
        return compute(data.data(), data.size());
    }

    std::pair<uint8_t, uint8_t> make_trailer_be(const uint8_t* data, size_t len) const;
    std::pair<uint8_t, uint8_t> make_trailer_le(const uint8_t* data, size_t len) const;

private:
    uint16_t poly_;
    uint16_t init_;
    uint16_t xorout_;
    bool reflect_in_;
    bool reflect_out_;
};

class LfsrWhitening {
public:
    static LfsrWhitening pn9_default();

    void apply(uint8_t* data, size_t len);

private:
    explicit LfsrWhitening(uint8_t state) : state_(state) {}

    uint8_t step();

    uint8_t state_;
};

constexpr size_t kWhiteningSeqLen = 255;
const std::array<uint8_t, kWhiteningSeqLen>& whitening_sequence();

void dewhiten_payload(std::span<uint8_t> data, size_t offset = 0);

} // namespace lora::rx::gr
