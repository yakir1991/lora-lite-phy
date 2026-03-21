# Stage 5 – System Validation & Field Testing (Simulation-First)

Goal: Validate the standalone PHY end-to-end, exercising long-running workloads, BER/PER sweeps, and interoperability scenarios entirely via simulators until hardware becomes available.

## 1. Conformance & Regression Campaign
- [x] Expand the regression harness to cover BER/PER sweeps across SF/BW/CR matrices using the captured golden vectors (float + Q15 paths).  
  - `tools/run_ber_sweep.py` drives AWGN sweeps (see `docs/stage5_ber_matrix.json`) and emits aggregated JSON/CSV results; lightweight coverage runs via the `host_sim_ber_sweep` CTest target.
- [x] Integrate the sweep runner into CI (nightly/weekly) so drift in fixed-point parity or real-time deadlines fails the pipeline.  
  - `tools/compare_ber_results.py` enforces tolerances against `docs/stage5_ber_baseline.json` (`host_sim_ber_compare` in CI); broader coverage runs via `host_sim_ber_sweep_nightly` (matrix `docs/stage5_ber_matrix_nightly.json`).

## 2. Interoperability via Simulation
- [x] Run compatibility simulations against GNU Radio reference blocks: feed shared captures through both stacks, diff at packet level, and record pass/fail.  
  - `tools/run_interop_compare.py` decodes each capture via GNU Radio (`tools/gr_decode_capture.py`) and the standalone pipeline (`lora_replay --dump-payload`), emitting mismatch counts; `host_sim_interop_compare` executes a representative pair in CI.
- [ ] Generate “field” traffic from public LoRaWAN traces (converted to IQ via the reference chain) to widen coverage.
- [x] Add latency/robustness benchmarking versus GNU Radio under CFO/STO/SFO drift.  
  - See `tools/run_receiver_vs_gnuradio.py` (matrix in `docs/receiver_vs_gnuradio_matrix.json`) and the latest report `docs/gnu_radio_comparison.md`. Coverage now spans SF5–SF12, implicit-header/no-CRC variants, and multiple impairment profiles (CFO/SFO, low SNR, burst noise, collision overlay) while also collecting RAM/CPU metrics for both stacks.

## 3. Stress & Resilience Soak
- [x] Schedule periodic `ctest -L host_sim_soak` runs to catch long-duration regressions on the live streaming path.  
  - `host_sim_live_soak_sf7` / `_sf9` (capture-driven, impairment-aware) emit `build/stage5_live_soak_sf7_metrics.json` and `build/stage5_live_soak_sf9_metrics.json`, covering both short/long symbol regimes with MTBF timestamps and run duration.
- [x] Add targeted impairments (frequency drift, burst interference) during soak runs to mimic harsh channel conditions; record MTBF-style metrics (time-to-failure) for future comparison once hardware tests start.

## 4. Documentation & Checklists
- [x] Produce a simulation validation checklist (test matrix, expected outcomes, log locations) so results can be reviewed quickly.  
  - See `docs/validation_playbook.md` for ctest shortcuts, scripts, and output paths.
- [x] Maintain dashboards (Markdown + CSV/JSON) summarising: BER curves, deadline margins, soak stability, and overall pass/fail.  
  - `tools/summarise_validation.py` emits `build/stage5_validation_summary.md` (includes BER, interop, soak metrics).

## 5. Hardware Bridging Plan (Prep)
- [x] Enumerate the artefacts needed for eventual hardware trials (scripted capture playback, configuration snapshots, SoapySDR adapter usage).  
  - See `docs/stage5_hardware_bridging.md` for the pre-flight checklist and adapter guidance; OTA tasks remain pending hardware availability.
