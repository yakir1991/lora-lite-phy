#include <iostream>
#include <vector>
#include <random>
#include "whitening.hpp"
#include "crc16.hpp"
#include "gray.hpp"
#include "hamming.hpp"
#include "interleaver.hpp"

using namespace lora_lite;

int main() {
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> dist(0,255);
    std::vector<uint8_t> payload(16);
    for (auto &b: payload) b = static_cast<uint8_t>(dist(rng));

    auto w = whiten(payload);
    auto crc = crc16_ibm(w);

    std::cout << "Payload size=" << payload.size() << " CRC=0x" << std::hex << crc << std::dec << "\n";
    return 0;
}
