# GNU Radio Compatibility Guide# GNU Radio Co## Implementation Summary



This document explains the current status of LoRa PHY compatibility with GNU Radio's gr-lora-sdr implementation.**Python Receiver (`complete_lora_receiver.py`):**

- ✅ 100% GNU Radio - **Performance:*## Testing Framework

**Updated:** October 2025

**Automated Validation:**

## Implementation Summary- `test_gnu_radio_compat.py` - End-to-end compatibility tests

- `pytest.ini` - Test configuration and parameters  

**Python Receiver (`complete_lora_receiver.py`):**- Golden vector validation against GNU Radio reference

- ✅ 100% GNU Radio compatibility achieved- No oracle assists or hardcoded shortcuts for unbiased testing Capabilities:**

- ✅ Full preamble/sync detection  - Stage-by-stage processing dumps with `--gr-dump-stages`

- ✅ Header parsing with variant search- Symbol alignment verification against GNU Radio FFT demod

- ✅ Payload decoding with CRC validation- Header and payload bit mapping analysis  

- ✅ Produces identical payloads to GNU Radio reference- CRC validation and error reporting



**C++ Receiver (`ReceiverLite`):****Expected Results:**

- ✅ Frame synchronization with CFO estimation- Python: 100% GNU Radio compatibility (✅ HELLO_WORLD: `48454c4c4f5f574f524c44`)

- ✅ Header detection and variant search - C++: Frame sync working, header selection under refinemention after behavior verification

- 🔄 Header selection stability (under debugging)

- ✅ Comprehensive deinterleaving and whitening## Testing Framework

- ✅ CRC validation with robust length search

**Automated Validation:**

**Current Status:** We have successfully implemented both Python and C++ LoRa receivers with full preamble detection, header parsing, and payload decoding. The Python implementation achieves 100% compatibility with GNU Radio reference, while the C++ implementation is undergoing final debugging for payload parity.- `test_gnu_radio_compat.py` - End-to-end compatibility tests

- `pytest.ini` - Test configuration and parameters  

## Validation Tools- Golden vector validation against GNU Radio referenceible

- ✅ Full preamble/sync detection  

**Python-vs-C++ Comparator (`scripts/compare_py_vs_cpp.py`):**- ✅ Header parsing with variant search

```bash- ✅ Payload decoding with CRC validation

python3 scripts/compare_py_vs_cpp.py <vector.cf32> --sf 7 --bw 125000 --cr 2 --has-crc --ldro 2- ✅ Produces identical payloads to GNU Radio reference

```

- Runs both Python and C++ receivers on the same vector**C++ Receiver (`ReceiverLite`):**

- Reports payload match status and debugging information- ✅ Frame synchronization with CFO estimation

- Essential for validating C++ implementation parity- ✅ Header detection and variant search 

- 🔄 Payload mapping (under debugging)

**Hybrid Receiver (`scripts/hybrid_receiver.py`):**- ✅ Comprehensive deinterleaving and whitening

```bash- ✅ CRC validation with robust length searchbility Status

python3 scripts/hybrid_receiver.py <vector.cf32> --sf 7 --bw 125000 --cr 2 --has-crc

```This document explains the current status of LoRa PHY compatibility with GNU Radio's gr-lora-sdr implementation.

- Uses C++ for performance, falls back to Python if C++ fails

- Provides unified interface for both implementations**Current Status:** We have successfully implemented both Python and C++ LoRa receivers with full preamble detection, header parsing, and payload decoding. The Python implementation achieves 100% compatibility with GNU Radio reference, while the C++ implementation is undergoing final debugging for payload parity.



## Current Development Status**Updated:** October 3, 2025io Compatibility Guide



**Python Implementation (✅ Complete):**This document explains how we validate LoRa decoding against GNU Radio’s gr-lora-sdr and what we currently focus on.

- Full GNU Radio compatibility achieved

- Robust header and payload decodingCurrent focus: a hybrid Python receiver with targeted C++ assists for performance-critical pieces. Older scripts and analyses that do not contribute directly to parity have been removed or deprecated in this guide to keep things lean and actionable.

- All test vectors passing consistently

Updated: October 1, 2025

**C++ Implementation (🔄 Debugging):**

- Frame synchronization: Working## What we are focusing on now

- Header detection: Working  

- Header selection: Under refinement (occasional incorrect length detection)- Hybrid receiver: Python-first pipeline that mirrors GNU Radio’s reference behavior, with C++ helpers when useful (e.g., sync/demod primitives).

- Payload decoding: Dependent on header stability- Reproducible, unbiased parity checks using the exact same input vectors and parameters that gr-lora-sdr expects.

- Tight inner-loop diagnostics: stage dumps, header mapping checks, and payload CRC validation.

**Test Coverage:**

- Golden vectors with known payloads (HELLO_WORLD)## Validation Tools

- Cross-validation between Python and C++ implementations

- GNU Radio reference compatibility verification**Python-vs-C++ Comparator (`scripts/compare_py_vs_cpp.py`):**

```bash

## Reference Integrationpython3 scripts/compare_py_vs_cpp.py <vector.cf32> --sf 7 --bw 125000 --cr 2 --has-crc --ldro 2

```

**GNU Radio Compatibility:**- Runs both Python and C++ receivers on the same vector

- `external/gr_lora_sdr/` contains the reference implementation- Reports payload match status and debugging information

- Used for validation and stage-by-stage comparison- Essential for validating C++ implementation parity

- Python implementation achieves 100% compatibility

**Hybrid Receiver (`scripts/hybrid_receiver.py`):**

**Test Vectors:**```bash

- `golden_vectors_demo/` and `golden_vectors_demo_batch/` contain validated test casespython3 scripts/hybrid_receiver.py <vector.cf32> --sf 7 --bw 125000 --cr 2 --has-crc

- Each vector includes both .cf32 signal data and .json metadata```

- Essential for automated testing and continuous integration- Uses C++ for performance, falls back to Python if C++ fails

- Provides unified interface for both implementations

## Quick Start

## Current Development Status

**Run Python receiver with GNU Radio comparison:**

```bash**Python Implementation (✅ Complete):**

python -m receiver.cli --sf 7 --bw 125000 --cr 2 --ldro-mode 2 --samp-rate 500000 \- Full GNU Radio compatibility achieved

  --compare-gnuradio golden_vectors_demo/tx_sf7_bw125000_cr2_crc1_impl0_ldro2_pay11.cf32- Robust header and payload decoding

```- All test vectors passing consistently



**Run Python vs C++ comparator:****C++ Implementation (🔄 Debugging):**

```bash- Frame synchronization: Working

python3 scripts/compare_py_vs_cpp.py golden_vectors_demo/tx_sf7_bw125000_cr2_crc1_impl0_ldro2_pay11.cf32 \- Header detection: Working  

  --sf 7 --bw 125000 --cr 2 --has-crc --ldro 2- Header selection: Under refinement (occasional incorrect length detection)

```- Payload decoding: Dependent on header stability



## Project Structure**Test Coverage:**

- Golden vectors with known payloads (HELLO_WORLD)

**Core Implementation:**- Cross-validation between Python and C++ implementations

- `receiver/` - Python LoRa decoder (100% GNU Radio compatible)- GNU Radio reference compatibility verification

- `src/` - C++ implementation (frame sync, header parsing, payload decoding)  

- `scripts/` - Validation and comparison tools## Reference Integration

- `golden_vectors_demo/` - Test vectors with known payloads

**GNU Radio Compatibility:**

**Key Files:**- `external/gr_lora_sdr/` contains the reference implementation

- `receiver/cli.py` - Python CLI with GNU Radio integration- Used for validation and stage-by-stage comparison

- `scripts/compare_py_vs_cpp.py` - Cross-implementation validator  - Python implementation achieves 100% compatibility

- `lora_decode_utils.py` - Core decode algorithms

- `src/receiver_lite_decode.cpp` - C++ payload processing**Test Vectors:**

- `golden_vectors_demo/` and `golden_vectors_demo_batch/` contain validated test cases

## Development Goals- Each vector includes both .cf32 signal data and .json metadata

- Essential for automated testing and continuous integration

- **Correctness First:** Exact GNU Radio payload compatibility  

- **No Shortcuts:** Avoid hardcoded positions or oracle assists## Quick Start

- **Robust Testing:** Comprehensive validation with golden vectors

- **Performance:** C++ optimization after behavior verification**Run Python receiver with GNU Radio comparison:**

```bash

## Testing Frameworkpython -m receiver.cli --sf 7 --bw 125000 --cr 2 --ldro-mode 2 --samp-rate 500000 \

  --compare-gnuradio golden_vectors_demo/tx_sf7_bw125000_cr2_crc1_impl0_ldro2_pay11.cf32

**Automated Validation:**```

- `test_gnu_radio_compat.py` - End-to-end compatibility tests

- `pytest.ini` - Test configuration and parameters  **Run Python vs C++ comparator:**

- Golden vector validation against GNU Radio reference```bash

- No oracle assists or hardcoded shortcuts for unbiased testingpython3 scripts/compare_py_vs_cpp.py golden_vectors_demo/tx_sf7_bw125000_cr2_crc1_impl0_ldro2_pay11.cf32 \

  --sf 7 --bw 125000 --cr 2 --has-crc --ldro 2

**Debug Capabilities:**```

- Stage-by-stage processing dumps with `--gr-dump-stages`

- Symbol alignment verification against GNU Radio FFT demod## Project Structure

- Header and payload bit mapping analysis  

- CRC validation and error reporting**Core Implementation:**

- `receiver/` - Python LoRa decoder (100% GNU Radio compatible)

**Expected Results:**- `src/` - C++ implementation (frame sync, header parsing, payload decoding)  

- Python: 100% GNU Radio compatibility (✅ HELLO_WORLD: `48454c4c4f5f574f524c44`)- `scripts/` - Validation and comparison tools

- C++: Frame sync working, header selection under refinement- `golden_vectors_demo/` - Test vectors with known payloads

**Key Files:**
- `receiver/cli.py` - Python CLI with GNU Radio integration
- `scripts/compare_py_vs_cpp.py` - Cross-implementation validator  
- `lora_decode_utils.py` - Core decode algorithms
- `src/receiver_lite_decode.cpp` - C++ payload processing

## Development Goals

- **Correctness First:** Exact GNU Radio payload compatibility  
- **No Shortcuts:** Avoid hardcoded positions or oracle assists
- **Robust Testing:** Comprehensive validation with golden vectors
- **Performance:** C++ optimization after behavior verification

## Goals and acceptance

- Produce exactly the same payload bytes as GNU Radio’s decoder for the supplied golden vectors (e.g., HELLO_WORLD for the demo file).
- Avoid biased shortcuts (no hardcoded positions or sync words). Use only sf/bw/cr/crc/impl/ldro and sample rate.
- Prefer correctness and diagnosability first; move hot paths to C++ once behavior is locked.

## Test integrity (unbiased checks)

- Our pytest (`test_gnu_radio_compat.py`) runs end-to-end on golden vectors and calls GNU Radio’s offline decoder with the same parameters. It does not pass hidden positions or use "oracle" assists.
- The Python CLI defaults keep `--oracle-assist` off and do not enable any known-position shortcuts unless explicitly set via an environment variable (`LORA_ALLOW_KNOWN_POS=1`).
- Sync words: tests and CLI accept `--sync-words`, and the Python receiver uses those values when searching for the sync sequence. If absent, it defaults to 0x12, matching common LoRa setups.

## Troubleshooting quick tips

- If the payload doesn’t match, enable stage dump and review:
  - Raw symbol alignment vs GNU Radio’s fft_demod_sym
  - Header Gray mapping (-44 shift, divide by 4, Gray demap with sf-2 bits)
  - Post-Hamming bit packing and whitening mode/seed
- Use the exact-L neighborhood search in the CLI to probe small orientation variants when the GR payload length is known.

## Notes

- This README intentionally omits older comparison and batch scripts that are no longer part of the primary loop. The focus is on the hybrid Python+C++ receiver and the two GNU Radio reference scripts above.