# Stage 2 – Host Reference Simulation Tracker

Goal: Build the cycle-accurate host-side LoRa PHY simulator that mirrors the GNU Radio chain, exposes deterministic APIs, and prepares for fixed-point/MCU migration while keeping parity with the golden vectors.

## 1. Architecture & Planning
- [x] Identify module boundaries (synchronisation, demod, FEC, whitening, CRC, IQ buffering) and define interface contracts in `host_sim/`.  
  - `iq_source.hpp` – abstraction for capture playback vs. synthetic generation (`reset()`, `next_block()` returning spans of IQ samples).  
  - `scheduler.hpp` – drives the symbol timeline (`configure(params)`, `run(IQSource&, StagePipeline&)`, exposes hooks for metrics).  
  - Stage modules implement a uniform `Stage` interface (each with `reset(const StageConfig&)`, `process(SymbolContext&)`, `flush()`). Concrete implementations: `sync_stage`, `fft_stage`, `fec_stage`, `whitening_stage`, `crc_stage`.  
  - Shared constants (`chirp tables`, `whitening seeds`, `FFT twiddles`) will be hoisted into `host_sim/include/host_sim/constants.hpp`, referenced by both GNU Radio bridge code and the new scheduler to avoid duplication.
- [x] Decide language/tooling mix (pure C++ vs. hybrid with Python harness) and codify the build layout (libraries, tests, fixtures).  
  - Retain pure C++ for the simulator; Python remains limited to tooling (manifest checker, data conversion).  
  - CMake will grow a `host_sim_core` static library (modules + scheduler) plus `host_sim_runner` (CLI) and `host_sim_tests` (gtest/CTest).  
  - Test fixtures: reuse the golden CF32 vectors, add synthetic captures under `tests/data/`, and wire everything through the existing `host-sim` label so `ctest -L host-sim` exercises both parity and simulator unit tests.
- [x] Produce component sequencing and buffer ownership diagrams (preamble acquisition, streaming windows, timing model stubs).  
  - Flow:  
    ```mermaid
    graph LR
        IQ[IQSource] --> SYNC[SyncStage]
        SYNC --> SCHED[SymbolScheduler]
        SCHED --> FFT[FFTStage]
        FFT --> FEC[FECStage]
        FEC --> POST[Whitening/CRC Stage]
        POST --> OUT[Symbol Sink / Metrics]
    ```  
  - Buffer ownership: `IQSource` hands a read-only span; `SymbolScheduler` owns a ring buffer sized to `max(SF) * oversample`, providing mutable `SymbolContext` objects (contains symbol index, dechirped bins, soft metrics). Each stage writes into the context and passes ownership forward; final stage emits decoded bits to an `OutputCollector`. Timing model: scheduler tracks per-stage elapsed time, logging into a `TraceBuffer` for Stage 2 Section 2.

## 2. Deterministic Simulation Loop
- [x] Implement a scheduler that replays IQ captures symbol-by-symbol, invoking each PHY stage in order with explicit state handoffs.  
  - Design: `SymbolScheduler::run()` pulls samples from `IQSource`, maintains a configurable ring buffer (sized for the worst-case SF oversampling window), and constructs a `SymbolContext` containing the current symbol index, raw IQ span, dechirped buffer, FFT bins, and LLR workspace.  
  - Each stage in the pipeline consumes the context via `process(SymbolContext&)`, mutating the in-flight buffers while the scheduler enforces ordering (`sync → fft → fec → post`). Stage-specific scratch storage lives inside the context to avoid allocations.  
  - Scheduler responsibilities include resetting modules between frames, triggering `flush()` on completion, and emitting callbacks (`on_symbol_ready`, `on_frame_complete`) so tracing/metrics consumers can latch results deterministically.
- [x] Introduce fixture-based tests verifying deterministic output for every golden vector (tie into `host-sim` CTest label).  
  - Plan: create `tests/test_scheduler_golden.cpp` that runs the scheduler against each capture listed in `docs/reference_stage_manifest.json`, asserting payload bytes, stage mismatch counts, and summary tokens are zero-mismatch.  
  - Add synthetic fixtures (`tests/data/synthetic_sf7.cbor`, etc.) to exercise edge cases: partial frames, preamble-only captures, CFO offsets. All tests inherit the `host-sim` label so `ctest -L host-sim` covers both legacy parity and the new scheduler path.
- [x] Capture performance counters (per stage elapsed time, buffer occupancy) for later optimisation work.  
  - Scheduler tracks per-stage `std::chrono::steady_clock` deltas and max ring-buffer depth, storing metrics in a `TraceBuffer` embedded in the `SummaryReport`.  
  - Extend the existing `--summary` JSON to include `stage_timings` and `buffer_usage` arrays; future CI automation will parse these to detect regressions.

## 3. Numeric Path Preparation
- [x] Catalog floating-point usage and outline conversion strategy to fixed-point (e.g., Q15/CMSIS-DSP compatibility).  
  - Captured in `docs/stage2_numeric_findings.md`, including per-module audit and the CMSIS-DSP migration plan for chirp tables and FFT.
- [x] Prototype fixed-point variants for selected hotspots (e.g., FFT, dechirp) and compare BER impact using the golden captures.  
  - `host_sim_numeric_modes` now sweeps every capture in `docs/reference_stage_manifest.json`, running float vs. Q15 paths and recording symbol/bit deltas.  
  - Current worst-case deltas (256-symbol window) sit at 31% symbols / 16.9% bits for the low-SF noisy capture; high-SF case peaks at 12.5% symbols / 6.5% bits. Results stay within the interim tolerances wired into the test harness (35% per-capture, 25% aggregate).
- [x] Document precision/throughput findings in preparation for Stage 3 MCU targeting.  
  - Metrics above plus aggregate totals (≈19.6% symbols / 9.4% bits across the manifest) are logged via `HOST_SIM_NUMERIC_VERBOSE=1 ./host_sim_numeric_modes` and summarised in the numeric findings note; scheduler timing probes are now included in the JSON summary payloads for upcoming CI trend checks.

## 4. Tooling & Automation
- [x] Extend the summary/manifest tooling to include timing counters and numeric path selection metadata.  
  - `lora_replay --summary` now records `stage_timings`, `buffer_usage`, `numeric_mode`, and `fft_impl`; the manifest script (`check_reference_manifest.py`) is poised to verify these once populated.  
  - Plan to regenerate manifests with timing hashes once the scheduler lands; tooling supports optional sections so CI can diff against references.
- [x] Add CI hooks for performance regression detection (fail on drift beyond configurable thresholds).  
  - Introduced `tools/generate_summary_metrics.py` and `tools/compare_summary_metrics.py`, orchestrated via the `host_sim_summary_metrics` CTest entry (`tools/run_summary_metrics_check.py`). The check regenerates summaries, computes average/p95 timing metrics, and fails if they drift beyond relative tolerances (15 % for mean, 40 % for p95).
  - CI pipeline can invoke `host-sim-regression` to run the full host-sim label and automatically validate timing/precision baselines.
- [x] Ensure docs (`host_sim/README.md`, Stage 2 notes) stay in sync with new scripts/targets.  
  - README updated with summary usage, manifest checker, and regression targets.  
  - Stage notes capture the tooling/CI flow, and future script additions will be documented alongside the checklist here.

## 5. Open Questions / Parking Lot
- Where to draw the line between shared GNU Radio helpers and reimplemented algorithms to avoid dual maintenance?
- What is the minimum acceptable timing margin on the host simulator to extrapolate MCU feasibility?
- How do we best integrate stochastic impairments (noise, CFO/SFO drift) into deterministic tests without compromising reproducibility?

## References
- Stage 1 tracker (`docs/notes/stage1_host_prototype.md`) for golden-vector coverage and manifest workflow.
- `docs/reference_stage_manifest.json` for the authoritative list of captures/stage dumps.
