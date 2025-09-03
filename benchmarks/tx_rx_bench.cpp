#include "lora/rx/loopback_rx.hpp"
#include "lora/tx/loopback_tx.hpp"
#include "lora/workspace.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <vector>

using namespace lora;
using namespace lora::utils;

// Simple global allocation counter via overridden new/delete
static std::atomic<size_t> g_alloc_count{0};

void* operator new(std::size_t sz) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* ptr = std::malloc(sz))
        return ptr;
    throw std::bad_alloc();
}

void* operator new[](std::size_t sz) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* ptr = std::malloc(sz))
        return ptr;
    throw std::bad_alloc();
}

void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }

static std::string cr_to_string(CodeRate cr) {
    switch (cr) {
    case CodeRate::CR45:
        return "4/5";
    case CodeRate::CR46:
        return "4/6";
    case CodeRate::CR47:
        return "4/7";
    case CodeRate::CR48:
        return "4/8";
    default:
        return "?";
    }
}

int main(int argc, char** argv) {
    int nframes = 1000;
    if (argc > 1)
        nframes = std::atoi(argv[1]);

    Workspace ws;
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> dist(0, 255);

    const std::vector<uint32_t> sfs = {7, 8, 9, 10, 11, 12};
    const std::vector<CodeRate> crs = {CodeRate::CR45, CodeRate::CR46, CodeRate::CR47, CodeRate::CR48};

    std::cout << "metric,value,sf,cr\n";

    for (auto sf : sfs) {
        for (auto cr : crs) {
            // Fixed payload length for benchmark
            const size_t payload_len = 16;
            std::vector<uint8_t> payload(payload_len);
            for (auto& b : payload)
                b = dist(rng);

            // Warm up to allocate buffers
            auto sig = tx::loopback_tx(ws, payload, sf, cr);
            rx::loopback_rx(ws, sig, sf, cr, payload_len);

            g_alloc_count.store(0, std::memory_order_relaxed);
            auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < nframes; ++i) {
                for (auto& b : payload)
                    b = dist(rng);
                auto txsig = tx::loopback_tx(ws, payload, sf, cr);
                auto rxres = rx::loopback_rx(ws, txsig, sf, cr, payload_len);
                (void)rxres;
            }
            auto end = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(end - start).count();
            double pps = nframes / secs;
            size_t allocs = g_alloc_count.load(std::memory_order_relaxed);

            assert(allocs == 0 && "unexpected allocation after init");

            std::cout << "packets_per_second," << pps << ',' << sf << ',' << cr_to_string(cr) << "\n";
            std::cout << "allocations," << allocs << ',' << sf << ',' << cr_to_string(cr) << "\n";
        }
    }
    return 0;
}

