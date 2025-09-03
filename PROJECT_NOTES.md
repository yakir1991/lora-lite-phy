# LoRa Lite C++ PHY — Project Log (NOTES)

> Rule: at the **end of every completed step**, append a new entry in this file with **Done** and **Next**.

---

## 2025-09-01 — Milestone 0: Project Skeleton & Policies

**Done**
- Confirmed original reference repo is `tapparelj/gr-lora_sdr` (read‑only; vectors only).
- Chosen environment: **VS Code on WSL2 (Ubuntu 22.04)**; packages: `build-essential`, `cmake`, `ninja-build`, `pkg-config`, `git`, `libliquid-dev`, `valgrind`, `ccache`, `clang-format`.
- Defined repository strategy: new repo `lora-lite-phy` that builds standalone (no GNU Radio).
- Agreed on MVP constraints: **float-only**, **single FFT backend (liquid-dsp)**, **one-time allocations** policy, **no changes** to the original repo.
- Agreed file/dir skeleton: `include/`, `src/` (workspace, fft_liquid, tx, rx, utils), `tests/`, `benchmarks/`, `cmake/`, `.vscode/`.

**Next**
- **Milestone 1 — Tables & Utilities**: implement and test
  - Whitening sequence
  - CRC (LoRa-compatible)
  - Gray mapping + inverse
  - Hamming encode/decode for CR 4/5, 4/6, 4/7, 4/8
  - Interleaver patterns
- Prepare minimal unit tests (gtest/ctest) and wire them into CMake.
- Lock initial parameters for bring-up: SF7, BW 125 kHz, CR 4/7, explicit header.

## 2025-09-01 — Milestone 1: CRC + Header CRC + Whitening

**Done**
- Added **CRC-16 CCITT** utility (poly `0x1021`) with `compute/verify` and a 2-byte **Big-Endian trailer** for the payload.
- Added **Header CRC** wrapper based on the same CRC (Explicit header); unit test passes.
- Updated **whitening** to an 8-bit LFSR (`x^8 + x^6 + x^5 + x^4 + 1`), producing one PRBS byte per data byte; round-trip test passes.
- Utilities scaffold in place: Gray (encode/decode + inverse table), Hamming (CR 4/5..4/8, placeholder), Interleaver (placeholder).
- Build & tests: **7/7 passed**.

**Next**
- **Cross-validate CRC** parameters (init/xorout/endianness) against vectors from `tapparelj/gr_lora_sdr`; adjust if needed.
- Replace placeholders with spec-accurate implementations:
  - **Hamming** for CR **4/5, 4/6, 4/7, 4/8** (encode/decode with error detect/correct where applicable) + unit tests.
  - **Diagonal interleaver** mapping (incl. **LDRO** handling for SF>6) + unit tests across SF7..SF12 and CRs.
- Add deterministic **known-good tests** for Hamming/Interleaver.
- Prepare **TX→RX loopback** (symbol-aligned IQ, no sync) integrating these utilities.
- Add a small **benchmark harness** (pps/cycles) and verify **zero allocations** on hot paths (valgrind/mallinfo).

## 2025-09-02 — Submodules pinned

**Done**
- Pinned `external/liquid-dsp` to **v1.7.0** (`4dda702…`).
- Pinned `external/gr_lora_sdr` to **a8143cb…** (reference vectors).
- `.gitmodules` set to **shallow** for faster clones; submodules kept out of the build.

**Next**
- Implement spec-accurate **Hamming** (CR 4/5..4/8) and **Diagonal Interleaver** (incl. LDRO) + unit tests (SF7..SF12).
- Add `scripts/export_vectors.sh` to generate known-good vectors from `gr_lora_sdr` for cross-validation.

## 2025-09-03 — Hamming codec complete

**Done**
- Replaced placeholder with generated Hamming (4,n) tables and syndrome maps.
- Added decode path correcting single-bit errors (CR4/7,4/8) and detecting errors for CR4/5 and CR4/6.
- Expanded unit tests to cover all coding rates.

**Next**
- Implement diagonal interleaver patterns and associated unit tests.
- Cross-validate utilities against reference vectors from `gr_lora_sdr`.

## 2025-09-04 — Interleaver patterns complete

**Done**
- Replaced placeholder interleaver with spec-accurate diagonal mapping including LDRO column shift.
- Documented interleaver API and dimensions.
- Added comprehensive unit tests for SF7–SF12 and CR 4/5–4/8 verifying round-trip and LDRO behaviour.

**Next**
- Cross-validate interleaver patterns against `gr_lora_sdr` vectors.
- Prepare TX→RX loopback harness integrating utilities.

## 2025-09-05 — Milestone 2: Loopback Harness

**Done**
- Added workspace with one-time allocation of chirps, buffers, and FFT plan.
- Implemented TX and RX loopback modules chaining whitening, Hamming FEC, interleaving, Gray map, and chirp modulation with reciprocal demodulation.
- Created end-to-end loopback test covering SF7–SF12 and CR 4/5–4/8.

**Next**
- Cross-validate against `gr_lora_sdr` vectors.

## 2025-09-06 — CRC cross-validation

**Done**
- Verified CRC-16 CCITT parameters against reference payload "123456789" (expected 0x29B1) and added golden unit test.

**Next**
- Export test vectors from `gr_lora_sdr` and validate TX/RX paths against them.

## 2025-09-07 — Cross-validation vectors

**Done**
- Exported reference TX IQ and payloads for SF7/CR4/5 and SF8/CR4/8 using `gr_lora_sdr`.
- Added automated test to check local TX/RX against these vectors.

**Next**
- Run AWGN robustness tests across coding rates.

## 2025-09-08 — Reference vector parity tests

**Done**
- Automated test validates that decoding the stored IQ yields the original payload and that re-encoding regenerates the reference IQ.

**Next**
- Sweep AWGN SNR to measure robustness across coding rates.

## 2025-09-09 — AWGN SNR Sweep

**Done**
- Added AWGN SNR sweep harness running TX→RX with Gaussian noise from 0–20 dB across CR 4/5–4/8.
- Wired the sweep into CMake and `ctest` for automated CI execution.

**Next**
  - Profile demodulator runtime to guide future optimizations.

## 2025-09-10 — Demodulator runtime profile

**Done**
- Instrumented RX demodulator with `std::chrono` and logged per-packet runtime.
- Wrapped loopback test with `valgrind --tool=massif`; peak heap usage ≈1.0 MB with average runtime ≈55 ms.

**Next**
- Examine and eliminate remaining dynamic allocations in the RX path.

## 2025-09-11 — RX path allocations removed

**Done**
- Added scratch buffers to `Workspace` and reused them in `loopback_rx` to avoid per-call dynamic allocations.

**Next**
- Consider exposing a view into the workspace buffer to eliminate the final output copy.

## 2025-09-12 — CI & devcontainer

**Done**
- Added GitHub Actions workflow that provisions GNU Radio, exports reference
  vectors and runs the test suite.
- Introduced VS Code devcontainer with GNU Radio and build dependencies
  preinstalled.
- Hardened `scripts/export_vectors.sh` with a Python override and GNU Radio
  preflight check.
- Documented CI vector generation, devcontainer usage and the `PYTHON`
  selector for the export script in the README.

**Next**
- Consider exposing a view into the workspace buffer to eliminate the final output copy.
