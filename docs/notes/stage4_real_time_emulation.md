# Stage 4 – Real-Time Emulation & Stress Testing Tracker

Goal: Emulate real-time streaming with the host simulator, stress the PHY under adverse conditions (jitter, interference, adaptive behaviour), and verify deadlines and robustness ahead of MCU deployment.

## 1. Real-Time Streaming Emulation
- [x] Implement a real-time driver that feeds IQ samples into the scheduler at wall-clock pace (configurable symbol rate) and asserts deadline adherence.  
  - `rt_driver.hpp`: spawns a timer thread that dispatches symbols based on `symbol_period = 2^SF / BW`; integrates with the cooperative scheduler hooks to sleep/yield between symbols.  
  - Driver records actual dispatch times vs. expected deadlines and writes `deadline_margin_us` metrics into `SummaryReport`.
- [x] Support live capture playback (e.g., from SDR) and loopback mode to test sustained throughput.  
  - `LiveSourceAdapter` consumes IQ from `SoapySDR` (or other SDR APIs) and feeds into `IQSource`; loopback mode reuses generated frames emitted by the PHY to measure end-to-end latency.  
  - CLI flags (`--rt-live`, `--rt-loopback`) select modes; tests simulate sustained streaming at max SF/BW combinations.
- [x] Log overrun/underrun events with timestamps and include them in the summary manifest.  
  - Driver emits `overrun_events` and `underrun_events` arrays (timestamp, symbol index, margin) in summary JSON.  
  - Overruns trigger CI failures once count exceeds threshold; metrics feed Stage 4 reporting.

## 2. Impairment Injection Harness
- [x] Integrate CFO/SFO drift, AWGN, burst noise, and packet collisions into the simulation pipeline with deterministic seeds.  
  - `impairments.hpp` module exposes knobs for CFO drift (ppm/sec), SFO drift, AWGN SNR, burst jammer profiles, and collision injection from recorded frames.  
  - Deterministic `std::mt19937` seeds ensure repeatability; impairments apply pre/post scheduler with context flags for downstream analysis.
- [x] Sweep impairment parameters across workloads and record acquisition time, tracking jitter, and PER/BER.  
  - Define sweep matrix: CFO drift {±0, ±5, ±10 ppm}, AWGN SNR {−5,0,+5 dB}, burst noise durations {0, 10, 50 symbols}, collision probability {0%, 10%, 25%}.  
  - Summary JSON records `acquisition_time_us`, `tracking_jitter_us`, `packet_error_rate`, `ber`. Results appended to `docs/stage4_sweep_results.json`.
- [x] Flag scenarios where tracking loops fail to converge and record mitigation strategies.  
  - Scheduler raises `tracking_failure` events with root cause (excess drift, burst length).  
  - Mitigation log suggests parameter adjustments (e.g., widen PLL bandwidth, increase preamble) and is captured in Stage 4 stress report.

## 3. Adaptive Behaviour Validation
- [x] Exercise dynamic SF/BW switching, adaptive power, and rate changes mid-stream; ensure state machine transitions stay deadline-safe.  
  - Scenario library triggers SF/BW changes based on impairment metrics (e.g., PER threshold, SNR dips).  
  - Scheduler logs transition latency, verifies no missed deadlines, and reports utilisation spikes in summary.
- [x] Validate that scheduler reconfiguration (e.g., LDRO toggles, CRC enable/disable) occurs without frame loss.  
  - Tests flip LDRO/CRC bits mid-frame and assert pipeline flush/reset preserves payload integrity; summary includes `reconfig_events` with status.
- [x] Document fallback behaviours when adaptation violates timing constraints.  
  - If `deadline_margin_us < 0` during adaptation, scheduler triggers fallback (revert previous SF/BW, extend preamble).  
  - Fallback actions logged in stress report and surfaced in summary JSON under `fallback_actions`.

## 4. Automation & Reporting
- [ ] Extend `--summary` to capture real-time metrics (overrun counts, worst-case latency, jitter statistics, PER under impairments).
- [ ] Add CI stress suites (`tools/run_rt_stress.py`) that execute the impairment matrix nightly and fail on regressions.
- [ ] Produce `docs/stage4_stress_report.md` summarising robustness findings and recommended operating envelopes.

## 5. Open Questions
- Do we need hardware-in-the-loop at this stage, or can SDR loopback cover all stress tests?
- What thresholds on jitter/overruns are acceptable for the target protocol layers?
- How should adaptive decisions be coordinated with higher-layer QoS constraints?
