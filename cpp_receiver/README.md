# LoRa C++ Receiver (Reboot)

This directory hosts an independent C++ LoRa receiver that draws from the proven Python `external/sdr_lora` pipeline and the GNU Radio implementation. Every stage is validated against both reference receivers before it moves forward.

## Current status

- **IQ Loader** (`IqLoader`): reads interleaved `cf32` IQ into `std::vector<std::complex<float>>`. Covered by tests that assert byte-for-byte agreement with NumPy.
- **Preamble Detector** (`PreambleDetector`): matched-filter search using a generated LoRa up-chirp. For the reference vector `sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown` it locks at offset `0` with metric `≈1.0`, matching the Python baseline.
- **Frame Sync** (`FrameSynchronizer`): replicates the GNU Radio-style coarse/fine preamble tracking to recover `p_ofs_est` and CFO (−244.140625 Hz for the reference vector), matching the Python receiver’s internal state.
- **Sync Word Detector** (`SyncWordDetector`): demodulates the eight preamble chirps plus the two sync chirps via a down-chirp FFT, reproducing the symbol bins `[0×8, 8, 16]` and GNU Radio-grade magnitudes seen in the Python receiver. The stage flags any preamble drift or sync-word mismatch.
- **Header Decoder** (`HeaderDecoder`): demodulates the eight explicit-header symbols using the frame-sync alignment, applies the LoRa Gray/interleave/Hamming pipeline, and recovers `length=18`, `CR=2`, `CRC=on` with raw header bins `[90, 122, 122, 126, 18, 110, 22, 78]`, identical to the Python implementation.
- **Payload Decoder** (`PayloadDecoder`): demods all payload symbols, replicates LoRa interleaver/Hamming/whitening, validates the CRC16, and outputs the exact `"hello stupid world"` bytes observed in the Python receiver.
- **Receiver façade** (`Receiver`): simple orchestrator that runs frame sync → header → payload stages and emits the decoded bytes/flags, making it easy to integrate with CLIs or future pipelines.
- **Streaming Receiver** (`StreamingReceiver`): incremental decoder that accepts arbitrary sample chunks, maintains rolling buffers, and emits structured events (`SyncAcquired`, `HeaderDecoded`, `PayloadByte`, `FrameDone`, `FrameError`). Ideal for long IQ recordings or real-time integrations where back-pressure is required.
- **CLI (`decode_cli`)**: command-line decoder that wraps the façade and prints sync/header/CRC status plus payload hex for any `.cf32` file.
- **Stage tests** (`run_stage_tests`): CMake target that covers loader, frame sync, header decode, payload decode, and sync analysis against the reference vector so regressions are caught immediately.

## Tools & scripts

- `cpp_receiver/build/decode_cli`: C++ parity checker (`--sf/--bw/--fs/--ldro` options, `--debug` for extra diagnostics).
- `cpp_receiver/tools/streaming_harness`: Feeds one or more captures into `StreamingReceiver`, synthesising configurable idle gaps and printing per-frame summaries to validate chunked decoding paths.
- `external/gr_lora_sdr/scripts/export_tx_reference_vector.py`: GNU Radio exporter used to generate golden vectors (SF/CR/LDRO sweeps, explicit/implicit headers, CRC toggles).
- Python reference: `external/sdr_lora/lora.decode` (kept as the oracle for comparison) plus helper scripts in `scripts/` (`sdr_lora_cli.py`, `compare_py_vs_cpp.py`, etc.).
- Golden vectors: new batch in `golden_vectors/new_batch/` (SF7–8, CR1–2, LDRO0/1/2, payload `HELLO_STUPID_WORLD`).

## Next steps

1. **Solidify low-SF support:** fix the SF5/6 header and payload paths (currently crash-prone) and add regression vectors to lock behaviour.
2. **Centralise shared DSP utilities:** extract the duplicated Gray de-mapping, DFT helpers, and timing constants into reusable components shared by all stages.
3. **Broaden automated regressions:** wire the extended golden-vector batches into the C++ stage tests and Python/GNU Radio parity suite so every configuration runs in CI.
4. **Document and harden streaming-specific workflows:** grow README coverage, keep the streaming harness in sync with CLI capabilities, and capture learnings from field testing now that the standalone plan document has been retired.

With the expanded vector library and existing parity harness in place, these milestones focus on robustness and maintainability as we converge on a production-grade native receiver.

## Embedded-Friendly Refactor Guide

This section captures concrete adjustments that make the receiver easier to integrate into embedded or resource-sensitive environments, even before picking a specific MCU. The focus is on refactoring strategy, modularity, and identifying heavy dependencies.

### 1. Slim the Public Surface
- Define a minimal API surface in `receiver.hpp` and `streaming_receiver.hpp` tailored for firmware: avoid STL-heavy structures in the interface, favour POD-style configs and preallocated buffers.
- Split utility code into `embedded/` modules so firmware builds can link only the required pieces (frame sync, header, payload, streaming).
- Provide compile-time flags (e.g., `LORA_EMBEDDED_MODE`) to disable host tooling (`IqLoader`, CLI helpers, filesystem access).

### 2. Control Dynamic Memory
- Audit all `std::vector` usage. Introduce fixed-capacity buffers sized by maximum supported SF/BW/payload, possibly via `StaticVector` wrappers or templates.
- Replace heap allocations in hot paths with preallocated workspaces passed from the caller (FFT scratch, dechirp buffers, whitening matrices).
- Offer `init()`/`reset()` APIs so firmware can reuse the same object with static storage across frames.

### 3. Optional Dependencies and Feature Flags
- Make Liquid-DSP optional at compile time. Provide fallback paths (already present) and allow a build without pkg-config. Encapsulate these choices behind `LORA_ENABLE_LIQUID_DSP` so embedded builds can switch them off.
- Wrap `<filesystem>`, `<thread>`, and exceptions behind macros. For embedded builds, disable them and supply lightweight replacements (`constexpr` paths, simple error enums).

### 4. Numerical Efficiency
- Evaluate precision requirements: keep `float`-based sample pipelines where possible, reserve `double` for precomputed chirps if needed. Document the trade-offs.
- Precompute chirp tables at compile time using `constexpr` functions or offline tools; store them in ROM/flash-friendly layouts to avoid runtime trig calls.
- Review FFT lengths: ensure they match powers of two compatible with CMSIS-DSP or alternative embedded FFT libraries.

### 5. Streaming Architecture
- Harden `StreamingReceiver` for chunked input from DMA/ISR sources: convert the internal buffer to a deterministic ring buffer with explicit thresholds.
- Expose callbacks or weak references so applications can react to `FrameEvent` without allocating (e.g., by passing a delegate struct with function pointers).
- Allow compile-time removal of event types you do not need to reduce code size.

### 6. Configuration and Build Profiles
- Add a dedicated CMake preset (e.g., `cmake --preset embedded`) that disables tools/tests, enables static buffers, and pulls in the embedded headers only.
- Provide an `embedded_config.hpp` template with tunable limits (max spreading factor, max payload bytes, symbol oversampling) that downstream users can tweak per platform.
- Document the expected RAM/flash usage per configuration so integrators can right-size their platforms.

### 7. Diagnostics and Logging
- Swap `std::cout`/`std::cerr` usage with an abstract logging interface. For embedded builds, implement a no-op logger or a UART-backed logger with compile-time gating.
- Provide optional counters (sync successes, CRC failures) stored in a small stats struct for field telemetry without bloating logs.

### 8. Testing Strategy
- Continue running existing unit tests on host to guard functionality. Mirror them with parameterized tests that use the embedded configuration to catch divergence early.
- Add integration tests that simulate constrained buffer sizes and verify deterministic behaviour (no reserve/resize calls, bounded memory footprint).
- Consider fuzzing headers/payload sequences to ensure error paths avoid undefined behaviour without relying on exceptions.

### 9. Integration Playbook
- Provide reference glue code under `examples/embedded_stub/` that feeds synthetic IQ chunks into `StreamingReceiver` and exposes decoded payloads through a callback API.
- Document initialization order: configure chirp tables, allocate buffers, reset state, then stream samples.
- Supply a checklist covering clock accuracy requirements, CFO budgets, and timing constraints so firmware engineers understand integration expectations.
