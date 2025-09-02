# LoRa Lightweight C++ PHY (No GNU Radio) — Migration Plan

**Goal:** Build a small, efficient, single-backend LoRa® PHY in C++ that runs without GNU Radio and **does not modify or break** the original `gr-lora_sdr` repository.  
**Status target:** First get a clean, minimal MVP working (float only, single FFT backend, one-time memory allocation). Only after the MVP is stable, consider expansions (fixed-point, additional FFT backends, hardware integration).

---

## Core Principles (MVP)
1. **No forking branches or feature flags** in the initial code. Keep the code path singular and simple.
2. **Float-only** signal path. No Q15/fixed-point in the MVP.
3. **One FFT backend** only. Choose an easy, proven backend: **liquid-dsp** FFT.
   - Upstream link: https://github.com/jgaeddert/liquid-dsp
4. **One-time allocation policy**: allocate all buffers once during initialization (including FFT plans and reference chirps). No dynamic allocations in the hot path. Release everything at shutdown.
5. **Zero dependency on GNU Radio**: the new code must compile and run independently, without linking to the original OOT module.
6. **Do not modify the original repository** (`tapparelj/gr-lora_sdr`). Treat it as a reference source for behavior and test vectors only.

---

## Repository Strategy (to avoid breaking the original code)
- **Option A (preferred):** Create a *separate* repository named, for example, `lora-lite-phy`. Keep `gr-lora_sdr` as a read-only reference (optionally as a Git submodule for test-vector generation only). This guarantees zero impact on the original code.
- **Option B (if it must live in the same repo):** Add a top-level sibling folder (e.g., `lite/`) with its own independent build files. Do **not** touch the top-level CMake or source tree used by `gr-lora_sdr`. The new folder should build in isolation and never run as part of the original CI.
- Either way, all integrations with the original project should be via **exported reference vectors** (IQ and bits) and not by sharing headers or linking symbols.

---

## MVP Scope
- **Supported PHY parameters (static for MVP):**
  - Spreading factor range: SF7 to SF12 (start with one SF for bring-up, then expand).
  - Bandwidths: start with 125 kHz; add 250 kHz and 500 kHz later.
  - Coding rate modes: 4/5, 4/6, 4/7, 4/8. (Note: only 4/7 and 4/8 correct single-bit errors; 4/5 and 4/6 are primarily error-detecting.)
  - Explicit header mode; implicit can be added after MVP.
  - Sync word and CRC support.
  - LDRO: considered later, after a working MVP.
- **Data path for TX:**
  1) Whitening  
  2) Hamming encoding according to CR  
  3) Interleaving (diagonal)  
  4) Gray mapping  
  5) Symbol-to-chirp modulation using precomputed reference upchirps
- **Data path for RX (aligned symbols; synchronization added later):**
  1) Dechirp using precomputed downchirp (complex conjugate)  
  2) FFT (size equals 2^SF)  
  3) Symbol decision (argmax bin; soft metrics optional later)  
  4) Gray demapping  
  5) Deinterleaving  
  6) Hamming decoding  
  7) Dewhitening and CRC validation

---

## Architecture (module boundaries, not code)
- **Configuration**: handles SF, CR, bandwidth, samples per symbol (start with one sample per symbol in MVP), header mode, sync word.
- **Workspace (one-time allocations)**: owns all buffers (FFT input/output, reference chirps up and down, temporary complex and real buffers, interleaver scratch, FEC scratch). Lifetime equals the context lifetime.
- **FFT backend (liquid-dsp)**: single plan created once for the chosen NFFT. Only execute during processing. No per-call plan creation.
- **TX logic**: pure functions operating on pre-allocated buffers. No allocations. Produces a vector or pointer range referencing pre-allocated memory.
- **RX logic**: consumes contiguous or block-wise IQ samples. Assumes symbol alignment at first; synchronization is a follow-up milestone. Produces decoded payload bytes and status flags via pre-allocated buffers.
- **Utilities**: CRC, whitening sequence, Gray tables, Hamming coding tables, interleaving patterns, and any lookup tables required by the demapper.

---

## Memory Model (no runtime allocations)
- **Initialization step**: allocate all required buffers based on maximum payload length and selected PHY parameters. Precompute and store:
  - Reference chirps (upchirp and downchirp) for the active SF.
  - Interleaver mapping tables per CR.
  - Hamming encode/decode tables and syndrome mapping.
  - Gray mapping and inverse mapping tables.
  - FFT plan and aligned buffers.
- **Processing steps** (TX and RX): only read/write into the existing buffers. Never call `malloc`, `new`, or free functions.  
- **Shutdown step**: free all buffers and destroy the FFT plan.

---

## FFT Backend Choice (single backend for MVP)
- Use **liquid-dsp FFT** as the baseline. It is battle-tested and straightforward to set up.
  - Upstream: https://github.com/jgaeddert/liquid-dsp
- Defer any alternative backends (KissFFT, FFTW, CMSIS-DSP) to *post-MVP*.
- The plan must be created once, and reused for every symbol. No plan recreation during processing.

---

## Synchronization (post-MVP add-on)
- Start the MVP with **aligned symbols** to simplify bring-up.
- After the MVP loopback is stable, add:
  - Preamble detection (correlation with upchirps)
  - STO (timing) estimation from peak location
  - CFO estimation from phase drift across chirps
  - Compensation before dechirp and FFT
- Maintain the one-time allocation policy by precomputing any required phasors and lookup tables.

---

## Validation & Test Plan
1. **Deterministic loopback (no noise):**  
   - Generate random payloads; run TX → RX with perfect symbol cuts; verify payload equality and CRC pass.
2. **Golden vectors (cross-project):**  
   - From `gr-lora_sdr`, export reference IQ for specific parameter sets (SF, CR, BW, payloads) and compare:
     - New RX must decode the reference TX IQ.
     - New TX followed by new RX must reproduce the original payload.
3. **AWGN tests:**  
   - Inject additive white Gaussian noise at controlled SNRs; measure BER/FER vs. SNR curves for the different coding rates. Expect 4/7 and 4/8 to outperform 4/5 and 4/6 in error correction.
4. **Regression set:**  
   - Unit-style tests for CRC, whitening, Gray mapping, Hamming coding/decoding, interleaving, and symbol decision.
5. **Performance checks:**  
   - Measure packets-per-second, CPU cycles (if available), and confirm zero runtime allocations by instrumenting allocation hooks during test runs.

---

## CI and Tooling (no code, just expectations)
- **Build on Linux** with standard toolchains. No GNU Radio installed or required.
- **Artifact checks**: run the unit-style tests and the loopback tests on CI.
- **Optional job** (manual or separate): generate or verify against `gr-lora_sdr` reference vectors on a machine where GNU Radio is available. Keep this job isolated so it cannot affect the main build.
- **Style & docs**: ensure consistent naming and documentation for all modules. Provide a clear “How to run the tests” section in the user docs once implemented.

---

## Work Breakdown & Milestones (MVP first)
**Milestone 0 – Project skeleton & policies (0.5–1 day)**  
- Create a new repository or a sibling folder that builds independently.  
- Document the “one-time allocation” rule and the single-backend decision.

**Milestone 1 – Tables and utilities (1–2 days)**  
- Whitening sequence, CRC, Gray mapping, Hamming tables, interleaver patterns.  
- Document parameter coverage and test with small, deterministic inputs.

**Milestone 2 – FFT integration and chirps (1–2 days)**  
- Integrate liquid-dsp FFT as the only backend.  
- Precompute upchirp and downchirp for the selected SF.  
- Confirm plan reuse and zero allocations during processing.

**Milestone 3 – TX path (1–2 days)**  
- Implement full TX pipeline over pre-allocated buffers.  
- Produce IQ for a dummy payload; verify basic invariants (power, symbol counts).

**Milestone 4 – RX path without synchronization (2–3 days)**  
- Implement dechirp, FFT, bin decision, Gray demap, deinterleave, Hamming decode, dewhitening, CRC.  
- Validate deterministic loopback end-to-end (no noise).

**Milestone 5 – Cross-validation with `gr-lora_sdr` (2–3 days)**  
- Export fixed test vectors from the original repo and verify decode/encode parity.  
- Fix any discrepancies until the new path matches within expected tolerance.

**Milestone 6 – Basic performance run (1 day)**  
- Measure throughput and confirm no allocations occur in the hot path.  
- Capture baseline numbers for future optimization.

**Milestone 7 – Synchronization (post-MVP) (3–5 days)**  
- Add preamble detection, STO/CFO estimation, and compensation.  
- Validate on noisier scenarios and with slight timing/frequency offsets.

---

## Risks and Mitigations
- **Hidden assumptions in the original flowgraphs:** rely on exported, parameterized test vectors; compare intermediate results (e.g., symbol decisions) to local expectations.  
- **Numerical differences in FFT or phase handling:** lock down reference chirp formulas, windowing choices, and bin selection rules; use the same FFT length and scaling consistently.  
- **CR behavior differences:** ensure Hamming encode/decode and interleaver geometry exactly match LoRa’s spec; write decoding syndrome tests early.

---

## Acceptance Criteria (MVP)
- End-to-end loopback passes for targeted SF/CR/BW combinations with CRC success.  
- Cross-validation passes against a fixed set of `gr-lora_sdr` vectors.  
- No malloc/new in processing calls after initialization.  
- The original repository remains untouched and fully functional.

---

## Post-MVP Ideas (defer until stable MVP)
- Additional FFT backends (KissFFT, FFTW, CMSIS-DSP).  
- Fixed-point (Q15) path for embedded targets.  
- Symbol synchronization refinements and LDRO automation.  
- Hardware radio integration (e.g., SDR capture, SX1262 testing harness).

---

**Reference:**  
- liquid-dsp (FFT and DSP library): https://github.com/jgaeddert/liquid-dsp

