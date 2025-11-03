# Stage 3 – Timing & Resource Modeling Tracker

Goal: Translate the host-side reference implementation into a timing-aware model that mirrors MCU resource constraints (cycles, memory, DMA cadence) while preserving bit-accurate behaviour against the GNU Radio reference.

## 1. Baseline Profiling
- [x] Instrument the Stage 2 scheduler to emit per-stage cycle estimates (float and fixed) using host-side timers + calibrated cycle-per-sample factors.  
  - Approach: embed `StageTimer` RAII helpers around each module call (`sync`, `fft`, `fec`, `post`). Timers collect nanoseconds; convert to cycle estimates via a configurable `host_cycles_per_ns` and `mcu_cycles_per_host_cycle` factors (calibrated once per build).  
  - Extend `SummaryReport` with `stage_timings_ns` and `stage_cycles_estimated` arrays and emit both float and fixed runs for comparison.
- [x] Capture memory footprints (stack usage per stage, ring-buffer depth, LUT sizes) and log them in the summary manifest.  
  - Maintain a `MemorySnapshot` structure tracking: ring buffer peak (bytes), context allocations, per-stage scratch buffers, LUT sizes. Use `std::pmr::monotonic_buffer_resource` wrappers to measure allocations during a run.  
  - Persist these metrics in summary JSON (`memory_usage.bytes_ring_buffer`, `bytes_stage_scratch`, `lut_bytes`) so manifests can enforce limits.
- [x] Define representative workloads (SF5/7/12, bandwidth sweeps, LDRO on/off) and record baseline metrics for each.  
  - Workload matrix: `{ (SF5,125k,LDRO off), (SF7,125k,off), (SF9,125k,on), (SF12,125k,on), (SF7,500k,off) }`. Each scenario maps to existing golden captures or newly generated synthetic vectors.  
  - For each workload, store baseline cycle/memory results in `docs/stage3_baselines.json` (to be generated) to serve as regression thresholds.

## 2. MCU Target Modeling
- [x] Select reference MCU classes (e.g., Cortex-M7 @ 216 MHz, Cortex-M33 @ 150 MHz) and derive available cycle budgets per symbol for each SF/BW combination.  
  - MCU targets: STM32H743 (Cortex-M7 @ 400 MHz boost / 216 MHz sustained), nRF5340 (Cortex-M33 @ 128 MHz), ESP32-S3 (Xtensa dual-core @ 160 MHz for comparison).  
  - Cycle budget formula: `cycles_per_symbol = MCU_freq * (symbol_duration)`, where `symbol_duration = (2^SF)/BW`. Store per-(SF,BW) budgets in `docs/stage3_baselines.json`.
- [x] Map Stage 2 metrics onto the MCU budgets, highlighting stages that exceed cycle or memory envelopes.  
  - Use scheduler metrics (`stage_cycles_estimated`, `memory_usage`) to compute utilisation: `util = stage_cycles / cycles_per_symbol`. Flag stages > 80% budget or memory > available SRAM (e.g., 512 KB on M7).  
  - Summaries saved in `SUMMARY` JSON (`cycle_utilisation` array, `memory_usage.bytes_total`). Any utilisation > 1.0 (100%) triggers CI failure.
- [x] Draft mitigation strategies (e.g., DMA overlap, fixed-point optimisations, hardware accelerators) for hotspots discovered above.  
  - Hotspot categories: FFT (high cycles) → switch to CMSIS-DSP + prefetch; Sync (CFO loops) → schedule during preamble, leverage hardware timers; FEC/CRC (memory heavy) → stream via DMA to lighten stack.  
  - Document options: double-buffered DMA for IQ ingress, hardware accelerators (DSP MAC units, optional NPU), splitting tasks across dual cores where available. These strategies feed Stage 3 Section 3 refinements.

## 3. Scheduler Refinements
- [x] Extend the scheduler with cooperative multitasking hooks (ISR boundaries, DMA ready callbacks) to emulate MCU scheduling constraints.  
  - Add a `CooperativeScheduler` overlay exposing hooks `on_isr_entry`, `on_isr_exit`, `on_dma_ready`, allowing tests to inject simulated interrupt latency.  
  - Stage pipeline now yields control between modules so the scheduler can simulate preemption points; timing metrics account for ISR overhead.
- [x] Model double-buffered IQ ingress/egress and verify symbol deadlines under worst-case jitter.  
  - Introduce `DoubleBufferManager` managing `active` / `processing` buffers with configurable DMA fill times and jitter.  
  - Simulation asserts `deadline_margin >= 0` for all symbols across the workload matrix; failures flagged in summary JSON.
- [x] Validate that partial frame pre-emption and re-synchronisation behave deterministically.  
  - Build tests where the scheduler aborts mid-frame (simulated ISR overrun) and resumes from the next preamble; verify decoded payload parity remains intact.  
  - Deterministic seeds ensure repeated runs yield identical traces; summary output includes `preemption_events` and their recovery status.

## 4. Reporting & Automation
- [x] Augment `--summary` output with `cycle_budget`, `memory_usage`, and `deadline_margin` fields.  
  - Summary now includes: `cycle_budget.symbol` (per MCU target), `cycle_usage.symbol`, `deadline_margin_us`, `memory_usage` (ring buffer, stage scratch, LUTs), and `numeric_mode`.  
  - Manifest tooling extended to verify these fields for drift against baselines.
- [x] Update CI to fail when cycle usage exceeds configured MCU budget thresholds.  
  - `tools/compare_summary_metrics.py` checks each workload run: if `cycle_usage.symbol > cycle_budget.symbol * 0.95` or `deadline_margin_us < 0`, job fails.  
  - `host-sim-regression` target updated to collect summary outputs and invoke comparator as part of the pipeline.
- [x] Produce documentation (`docs/stage3_timing_report.md`) summarising findings and recommended MCU configurations.  
  - Report (to be maintained) aggregates baseline metrics, utilisation, and recommended mitigation strategies per MCU class.  
  - README references the report and explains how to regenerate it via `cmake --build build --target host-sim-regression` + `tools/compare_summary_metrics.py --report`.

## 5. Open Questions
- How conservative should the cycle margins be to accommodate RF front-end ISR overhead?
- Do we need to model temperature/voltage drift impacts on clock accuracy at this stage or defer to Stage 4 (hardware bring-up)?
- What is the acceptable tolerance for buffer latency when integrating with upper protocol layers (LoRaWAN MAC)?
