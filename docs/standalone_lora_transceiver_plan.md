# Standalone LoRa Transceiver Roadmap (Software-First)

Goal: Derive a real-time capable LoRa transceiver stack independent of GNU Radio, validated end-to-end in simulation, and architected so it can later be ported onto a resource-constrained microcontroller with minimal rework.

## 0. Preparation & Knowledge Consolidation
- Re-read `docs/rev_eng_lora.md` and the accompanying PDFs to extract the exact modulation, framing, synchronization, and DSP equations. Distill them into implementation notes and state-machine diagrams.
- Catalogue reusable assets from `gr_lora_sdr` (algorithms, test vectors, parameter tables). Flag any GNU Radio assumptions (buffers, thread model, complex float usage) that must be replaced.
- Define success metrics for the eventual MCU target (max power, latency budget per symbol, RAM footprint ceiling). These constraints inform later optimization choices even while still in simulation.

## 1. Reference Simulation Environment (Host)
- Stand up a deterministic simulation harness in Python/C++ that can ingest captured IQ recordings and generate synthetic traffic. Leverage NumPy/scipy for prototyping while maintaining a thin abstraction layer so components can later be swapped for fixed-point equivalents.
- Mirror the full PHY pipeline: whitening, encoding, interleaving, modulation, synchronization (STO/CFO), demodulation, deinterleaving, decoding, and CRC checks. Keep modules functionally identical to `gr_lora_sdr`, but remove GNU Radio buffer semantics.
- Build unit and property tests that compare each stage against GNU Radio outputs for multiple spreading factors, coding rates, and bandwidths. Maintain golden vectors for regression.

_Status:_ Stage 1 delivered the comparison harness, manifest tooling, and regression targets. Stage 2 tracker (`docs/notes/stage2_reference_sim.md`) now drives the remaining host-side simulation architecture work.

## 2. Timing & Resource Modeling
- Instrument the host prototype to measure per-stage throughput and memory usage under worst-case SF/BW combinations. Translate floating-point operations into approximate fixed-point equivalents (MAC counts).
- Draft an execution schedule that meets symbol deadlines using a single-core deterministic loop. Identify natural task boundaries (e.g., preamble detection, symbol demod, FEC decode) for future ISR/DMA integration.
- Model buffering requirements (samples per symbol, overlap for FFT-based demod). Decide minimum double-buffer sizes and latency budgets to ensure porting feasibility.

_Status:_ Stage 3 tracker (`docs/notes/stage3_numeric_architecture.md`) captures the profiling tasks, MCU budget mapping, and scheduler refinements needed to close this section.

## 3. Hardware-Abstraction-Friendly Implementation
- Refactor simulation code into clear module boundaries with interfaces that accept raw buffers and configuration structs—no global state, no OS dependencies.
- Introduce compile-time switches or templates to toggle between float and fixed-point arithmetic. Prototype fixed-point variants in software and validate BER/SNR impact using Monte Carlo tests.
- Provide explicit hooks for hardware peripherals (RF front-end, DMA events) represented as mock interfaces in the simulator. This keeps firmware glue logic testable before silicon arrives.

## 4. Real-Time Emulation & Stress Testing
- Run closed-loop simulations that mimic real-time streaming: push IQ samples at the target sampling rate, enforce processing deadlines, and assert on overruns.
- Inject impairments (frequency offset, drift, channel noise, burst interference) to validate synchronization robustness. Capture metrics such as acquisition time, tracking jitter, and packet error rate.
- Explore adaptive behaviors (dynamic SF/BW, power adjustments) via simulation scenarios to ensure the architecture supports them without redesign.

_Status:_ Stage 4 tracker (`docs/notes/stage4_real_time_emulation.md`) captures the real-time streaming and stress testing workstream.
## 5. MCU Port Readiness Package
- Produce a portability dossier: required instruction set features, estimated RAM/flash budgets, recommended DMA/interrupt strategy, and external RF interface timing diagrams.
- Generate reference fixed-point lookup tables, twiddle factors, and whitening sequences sized for constrained memory.
- Document integration guidelines (build system layout, expected HAL APIs) so firmware engineers can start wiring the code once hardware is available.

## 6. Verification & Continuous Integration
- Automate the simulation test suite (including Monte Carlo BER sweeps and stress tests) in CI so every change maintains parity with the GNU Radio reference.
- Maintain differential tests between the float and fixed-point implementations to ensure numerical drift stays within acceptable limits.
- Track open questions or assumptions uncovered during simulation (e.g., PLL noise tolerance, crystal accuracy) and feed them into hardware bring-up planning.

Deliverables at the end of this roadmap: a fully simulated, timing-aware LoRa transceiver implementation with comprehensive tests, ready to be integrated into microcontroller firmware once hardware becomes available.
