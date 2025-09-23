#include "lora/rx/scheduler.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <complex>

using cfloat = std::complex<float>;

// Test Ring buffer functionality
void test_ring_buffer() {
    std::cout << "Testing Ring buffer..." << std::endl;
    
    Ring ring;
    
    // Test initial state
    assert(ring.avail() == 0);
    assert(ring.capacity() == MAX_RAW_SAMPLES);
    assert(ring.have(100) == false);
    
    // Test writing samples
    std::vector<cfloat> test_samples = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}};
    
    for (size_t i = 0; i < test_samples.size(); ++i) {
        assert(ring.can_write(1));
        cfloat* wptr = ring.wptr(ring.tail);
        assert(wptr != nullptr);
        *wptr = test_samples[i];
        ring.tail++;
    }
    
    assert(ring.avail() == test_samples.size());
    assert(ring.have(test_samples.size()));
    
    // Test reading samples
    const cfloat* rptr = ring.ptr(ring.head);
    assert(rptr != nullptr);
    assert(*rptr == test_samples[0]);
    
    // Test advance
    ring.advance(1);
    assert(ring.avail() == test_samples.size() - 1);
    
    std::cout << "Ring buffer tests passed!" << std::endl;
}

// Test RxConfig and utility functions
void test_rx_config() {
    std::cout << "Testing RxConfig..." << std::endl;
    
    RxConfig cfg;
    cfg.sf = 7;
    cfg.os = 2;
    cfg.ldro = false;
    cfg.cr_idx = 1;
    cfg.bandwidth_hz = 125000.0f;
    
    // Test N_per_symbol
    assert(N_per_symbol(7) == 128);
    assert(N_per_symbol(8) == 256);
    assert(N_per_symbol(12) == 4096);
    
    // Test dec_syms_to_raw_samples
    size_t raw_samples = dec_syms_to_raw_samples(10, cfg);
    assert(raw_samples == 10 * 128 * 2); // 10 symbols * 128 samples/symbol * 2 oversampling
    
    std::cout << "RxConfig tests passed!" << std::endl;
}

// Test Scheduler initialization
void test_scheduler_init() {
    std::cout << "Testing Scheduler initialization..." << std::endl;
    
    RxConfig cfg;
    cfg.sf = 7;
    cfg.os = 2;
    cfg.ldro = false;
    cfg.cr_idx = 1;
    cfg.bandwidth_hz = 125000.0f;
    
    Scheduler sch;
    sch.init(cfg);
    
    // Test that scheduler is initialized properly
    assert(sch.cfg.sf == 7);
    assert(sch.cfg.os == 2);
    assert(sch.st == RxState::SEARCH_PREAMBLE);
    
    // Test history and window calculations
    assert(sch.H_raw > 0);
    assert(sch.W_raw > 0);
    assert(sch.small_fail_step_raw > 0);
    
    std::cout << "Scheduler initialization tests passed!" << std::endl;
}

// Test DetectPreambleResult
void test_detect_preamble_result() {
    std::cout << "Testing DetectPreambleResult..." << std::endl;
    
    DetectPreambleResult result;
    assert(result.found == false);
    assert(result.preamble_start_raw == 0);
    assert(result.os == 1);
    assert(result.mu == 0.0f);
    assert(result.eps == 0.0f);
    assert(result.cfo_int == 0);
    assert(result.phase == 0);
    assert(result.cfo_estimate == 0.0f);
    assert(result.sto_estimate == 0);
    
    // Test setting values
    result.found = true;
    result.preamble_start_raw = 1000;
    result.os = 2;
    result.phase = 1;
    result.cfo_estimate = 0.1f;
    result.sto_estimate = 5;
    
    assert(result.found == true);
    assert(result.preamble_start_raw == 1000);
    assert(result.os == 2);
    assert(result.phase == 1);
    assert(result.cfo_estimate == 0.1f);
    assert(result.sto_estimate == 5);
    
    std::cout << "DetectPreambleResult tests passed!" << std::endl;
}

// Test HeaderResult
void test_header_result() {
    std::cout << "Testing HeaderResult..." << std::endl;
    
    HeaderResult result;
    assert(result.ok == false);
    assert(result.sf == 0);
    assert(result.cr_idx == 0);
    assert(result.ldro == false);
    assert(result.has_crc == false);
    assert(result.payload_len_bytes == 0);
    assert(result.header_syms == 0);
    
    // Test setting values
    result.ok = true;
    result.sf = 7;
    result.cr_idx = 1;
    result.ldro = false;
    result.has_crc = true;
    result.payload_len_bytes = 10;
    result.header_syms = 16;
    
    assert(result.ok == true);
    assert(result.sf == 7);
    assert(result.cr_idx == 1);
    assert(result.ldro == false);
    assert(result.has_crc == true);
    assert(result.payload_len_bytes == 10);
    assert(result.header_syms == 16);
    
    std::cout << "HeaderResult tests passed!" << std::endl;
}

// Test PayloadResult
void test_payload_result() {
    std::cout << "Testing PayloadResult..." << std::endl;
    
    PayloadResult result;
    assert(result.ok == false);
    assert(result.payload_syms == 0);
    assert(result.consumed_raw == 0);
    assert(result.crc_ok == false);
    
    // Test setting values
    result.ok = true;
    result.payload_syms = 20;
    result.consumed_raw = 1000;
    result.crc_ok = true;
    
    assert(result.ok == true);
    assert(result.payload_syms == 20);
    assert(result.consumed_raw == 1000);
    assert(result.crc_ok == true);
    
    std::cout << "PayloadResult tests passed!" << std::endl;
}

// Test expected_payload_symbols function
void test_expected_payload_symbols() {
    std::cout << "Testing expected_payload_symbols..." << std::endl;
    
    // Test with different parameters
    size_t symbols1 = expected_payload_symbols(10, 1, false, 7, true); // SF=7, CR=1, no LDRO, CRC on
    assert(symbols1 == 28);

    size_t symbols2 = expected_payload_symbols(10, 1, true, 7, true); // SF=7, CR=1, with LDRO, CRC on
    assert(symbols2 > 0);

    size_t symbols3 = expected_payload_symbols(10, 3, false, 8, true); // SF=8, CR=3, no LDRO, CRC on
    assert(symbols3 > 0);

    // LDRO should affect the calculation
    assert(symbols1 != symbols2);

    size_t symbols_no_crc = expected_payload_symbols(10, 1, false, 7, false); // CRC off
    assert(symbols_no_crc == 23);
    assert(symbols_no_crc < symbols1);
    
    std::cout << "expected_payload_symbols tests passed!" << std::endl;
}

int main() {
    std::cout << "=== Scheduler Unit Tests ===" << std::endl;
    
    try {
        test_ring_buffer();
        test_rx_config();
        test_scheduler_init();
        test_detect_preamble_result();
        test_header_result();
        test_payload_result();
        test_expected_payload_symbols();
        
        std::cout << "\n=== All tests passed! ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown test failure" << std::endl;
        return 1;
    }
}
