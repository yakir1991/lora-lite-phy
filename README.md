# LoRa Lite PHY — Standalone LoRa Receiver (C++)

A lightweight, GNU Radio–independent LoRa receive chain written in modern C++ (C++20) with a clear state-machine design. The work here uses gr-lora-sdr as a reference for algorithms and test vectors but does not depend on GNU Radio at runtime.

## Project goal

- Build a self-contained LoRa RX pipeline in C++ that can ingest IQ samples and output decoded frames (header fields and payload bytes) without GNU Radio.
- Keep it modular, readable, and testable; mirror the reference pipeline stages while minimizing external dependencies.

## What’s implemented

- DSP foundations
  - Reference upchirp/downchirp generation for a given SF
  - Decimation by oversampling factor and phase selection
  - Dechirp + FFT-based symbol demodulation (with tiny shift search)
  - Fractional CFO estimate from preamble and integer CFO bin estimate; both applied to demod results

- Detection and alignment
  - Preamble detection across OS candidates and phases
  - SFD detection via up/down classification
  - Header start location with small ±1 symbol search

- Header pipeline (explicit header)
  - Gray demap and LoRa-specific symbol shift handling
  - Reduced-rate header mapping s_hdr = ((Gray−1(bin) − 1) mod 2^sf) >> 2
  - Diagonal deinterleaver (sf_app = sf − 2)
  - Hamming(8,4) hard-decode using min-distance LUT (GR-compatible codewords)
  - Header checksum computation and validation
  - Row-rotation search to resolve header nibble ordering; robust field parsing

- Payload pipeline (in progress)
  - Gray demap + symbol alignment (payload path)
  - Diagonal deinterleaver (rows = sf, cw_len = 4 + cr)
  - Hamming(4,4+cr) hard-decode using LUTs (CR5-specific variant and cropped LUT for CR6–8)
  - Dewhitening using the standard sequence (bytes 0..payload_len−1); CRC bytes are not dewhitened
  - CRC16 (poly 0x1021, init 0x0000) verification logic implemented

- CLI tool
  - Reads interleaved float32 IQ (I then Q)
  - Prints header bins/bits and parsed header fields
  - Prints payload bytes (hex) and CRC status

## Key findings and decisions

- Gray demap and shift
  - The reference demapper applies a +1 shift in the Gray domain, but the effective LoRa symbol mapping used downstream is consistent with “mod(n − 1, 2^sf)” before reduced rate for the header. We aligned header and payload paths to use the same symbol alignment policy so downstream interleaving/decoding match GR ordering.

- Header checksum nuances
  - The explicit header checksum uses a particular parity over the three header nibbles. A small row-rotation across the deinterleaver output resolves residual ordering mismatches and yields valid checksums on provided vectors.

- Deinterleaver layout
  - Storing the interleaver matrix column-major and applying dest_row = mod(col − row − 1, rows) matches the GR implementation for both header and payload.

- Hamming decoding
  - Header: Hamming(8,4) decoded via min-distance lookup against the canonical GR LUT.
  - Payload: For CR=4/5 a distinct LUT is required; for CR=4/6–4/8 the same LUT is cropped to the codeword length.

- Whitening and CRC
  - Whitening applies to payload bytes only; CRC bytes are transmitted unwhitened in GR’s reference chain.
  - CRC16 uses poly 0x1021 and init 0x0000, with GR’s add/xor convention taken into account during verification on RX.

## Current status (Sep 2025)

- Header pipeline: robust and checksum-valid on test vectors in `vectors/`.
- Payload pipeline: decoding produces plausible bytes; CRC verification is still failing on at least one SF7/CR2 reference vector. Recent changes aligned symbol shift and nibble bit order with the reference; further reconciliation (whitening/CRC ordering and boundary handling) is underway.

## Repository layout

- `standalone/` — Self-contained CMake project with the RX core and a CLI
  - `include/` and `src/` hold the DSP, FEC, and state-machine code
  - `README.md` — quick start (older; superseded by this file for overall status)
- `external/gr_lora_sdr/` — Reference implementation (submodule) used for algorithms/test vectors
- `external/liquid-dsp/` — DSP primitives
- `vectors/` — IQ vectors and payload examples used for validation
- `docs/` — notes and design material

## Build and run

Prereqs: a C++20 compiler and CMake. From the repo root:

```bash
cmake -S standalone -B standalone/build
cmake --build standalone/build -j

# Run: iq_file (f32 interleaved IQ), sf, bw (Hz), fs (Hz)
standalone/build/lora_rx vectors/test_short_payload.unknown 7 125000 250000
```

## Next steps

1. Finalize payload path so CRC validates across the provided vectors
   - Reconcile dewhitening and CRC computation order to match GR’s add/xor convention precisely
   - Add targeted instrumentation to dump deinterleaver rows and nibbles for side-by-side diff vs GR
2. LDRO handling
   - Use reduced-rate (sf−2) where required on payload blocks when LDRO is active
3. Optional soft decisions
   - Add soft-decision Hamming decoding path for improved robustness
4. Tests
   - Unit tests for Hamming(8,4) and variable-CR payload LUT decoding
   - Golden-vector smoke tests for header fields and payload CRC
5. CLI polish
   - Switches for ASCII/hex, debug dumps, and concise frame summaries
6. Performance
   - Replace naive spots with well-tuned FFTs where helpful; add basic benchmarks

## Acknowledgements

- The design references algorithms and tables from [gr-lora-sdr](https://github.com/rpp0/gr-lora_sdr) and Semtech documentation. The external `gr_lora_sdr` directory is included here for study and vector generation.
