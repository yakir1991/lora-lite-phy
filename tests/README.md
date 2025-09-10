# Test Suite Overview

This repository includes several test categories. Use `ctest` to run them all
or filter by prefix/regex.

- Unit tests (utilities)
  - `Gray.*` — Gray encode/decode and inverse table
  - `Whitening.*` — PN9 whitening round-trip and regressions
  - `Hamming.*` — (4, n) encoder/decoder across CR45..CR48
  - `Interleaver.*` — diagonal interleaver round-trip and LDRO behavior
  - `CRC16.*` / `HeaderCRC.*` — payload/header CRC helpers

- PHY loopback
  - `Loopback.TxRx` — end-to-end TX→RX without synchronization (symbol-aligned)

- Synchronization
  - `Preamble.*` — preamble detection, CFO estimate/compensation, integer STO
  - `Preamble.OS4DetectAndDecode` — OS>1 support via decimation and phase search

- Reference vectors (optional GNU Radio)
  - `reference.*` — cross-validate against vectors under `vectors/`
  - `reference_os.*` — oversampled (OS=4) vectors via GNU Radio
  - `reference_os_hdr.*` — header-enabled OS=4 frames (preamble+sync+header+payload)
  - `reference_os_hdr_len.*` — like above, but sweeps multiple payload lengths (e.g., 16/24/31/48B)

- Performance
  - `AWGN.SNR_Sweep` — slow label; evaluates BER/FER vs SNR across coding rates

Notes
- `scripts/export_vectors.sh` generates and copies vectors into the build tree.
- Oversampled OS>1 decode first decimates to OS=1 with a Kaiser FIR (`firdecim_crcf`).
- Header-auto decode uses a streaming deinterleaver so header/payload are decoded
  from a single continuous bitstream without per-part padding.

Tips
- Run only synchronization tests: `ctest -R Preamble --output-on-failure`
- Run only reference vector tests: `ctest -R reference --output-on-failure`
- Run slow tests: `ctest -L slow --output-on-failure`

