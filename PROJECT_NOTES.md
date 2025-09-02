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
