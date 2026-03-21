# Stage 6 – Field Testing & Deployment Prep (Simulation-First Roadmap)

Goal: Define the workflows, tooling, and data collection needed once hardware/OTA trials begin, while preparing as much as possible using the current simulation assets.

## 1. Capture & Replay Infrastructure
- [x] Draft a capture catalogue format (`docs/captures/README.md` template) to track location, conditions, equipment, and metadata for each OTA recording.  
  - Prototype the JSON schema now using simulated captures from `gr_lora_sdr/data/generated`.
- [x] Extend `tools/run_interop_compare.py` to accept OTA capture directories so real-world recordings can be compared to GNU Radio and the standalone pipeline immediately after acquisition.
- [x] Teach `host_sim_live_soak` to ingest capture + metadata pairs via `--mode capture --capture <cf32> --metadata <json>` so the long-run soak path can replay OTA (or simulated) IQ with the same impairment knobs prior to hardware arrival.  
  - Seeded the catalogue with `docs/captures/session_sim_lab/` (currently SF7 + SF9 reference captures) and wired both soak CTest targets to those metadata files so CI now exercises multiple symbol-duration regimes.
- [x] Create a reproducible GNU Radio vs. standalone benchmarking harness (CFO/STO/SFO drift) so OTA captures can be compared apples-to-apples once they exist (`docs/gnu_radio_comparison.md`, `tools/run_receiver_vs_gnuradio.py`).

## 2. Simulation-to-Hardware Gap Analysis
- [ ] Document expected differences between the simulated impairment model and field impairments (phase noise, IQ imbalance, AGC transients).  
  - Create a tracking table outlining which metrics still rely on lab calibration (placeholder until hardware arrival).
- [ ] Prepare diagnostic hooks (log levels, symbol dumps) in `lora_replay` so OTA anomalies can be studied without recompiling.

## 3. Field Trial Playbooks
- [ ] Author a quick-start guide for on-site operators (bring-up checklist, required binaries, expected outputs).  
  - Base the CLI steps on `docs/validation_playbook.md`, annotating which depend on lab vs. field resources.
- [ ] Define acceptance criteria for the first OTA run (e.g., max PER, minimum RSSI, soak duration).

- [x] Prepare a notebook or script (`tools/analyse_ota.py`) to parse OTA capture logs, compute PER/BER, and compare against baselines stored under `build/`.  
  - Mock the pipeline now using simulated data to ensure the flow works end-to-end.
- [ ] Decide on long-term storage (artifact repository, S3 bucket) for OTA results; document retention policy.

## 4. Simulation-First Coverage Extensions (No Hardware Required)
- [x] Generate additional synthetic captures via `tools/generate_sim_captures.py` that hit corner cases (BW 62.5 kHz implicit header, SF12@BW500 kHz, payloads 128–256 B, CRC off) and wire them into `docs/receiver_vs_gnuradio_matrix.json`.
- [ ] Continue sweeping the generation script to cover:  
  - BW 62.5/250/500 kHz with CR 4/8 and LDRO on/off combinations.  
  - CRC-disabled, implicit-header, and very long payloads (≥512 B) for SF7–SF12.  
  - Extreme SNR scenarios (≤ –20 dB) plus mixed impairments (burst + collision + drift).  
- [ ] Expand impairment profiles in `tools/run_receiver_vs_gnuradio.py` with SFO drift, IQ imbalance approximations, and combined burst/collision modes so we can emulate OTA quirks before hardware exists.
- [ ] Instrument `host_sim/lora_replay` to emit per-stage timing and buffer metrics into its summary JSON; surface those in `docs/receiver_vs_gnuradio_summary.md` to pinpoint bottlenecks without lab gear.
- [ ] Automate Monte Carlo style runs (multiple random seeds per capture/profile) to quantify variance in decode time and PER wholly in simulation.
- [ ] Scripted CI batches: shard the comparison matrix into 2–3 capture groups per job (see section 5) so nightly runs update `docs/receiver_vs_gnuradio_results.json` and gate regressions in wall-time/RAM/CPU even before OTA data exists.
- [x] Name each scheduler stage in `lora_replay` (currently `Demod (float/Q15)`) so the instrumentation tables in the GNU Radio comparison report highlight real DSP blocks instead of generic `stage_0`.

## 5. Automation Hooks
- [x] Plan a CI job that, once OTA captures are available, periodically replays them through both GNU Radio and the standalone pipeline to guard against regressions.  
  - Job outline (nightly):
    1. `cmake --build build`
    2. `ctest -R host_sim_interop_compare --output-on-failure`
    3. `ctest -R host_sim_ber_sweep_nightly --output-on-failure`
    4. `ctest -L host_sim_soak --output-on-failure`
    5. Archive artifacts: `build/stage5_interop_results.json`, `build/stage5_ber_results_nightly.json`, `build/stage5_live_soak_{sf7,sf9}_metrics.json`
    6. Run the GNU Radio comparison sweep if capture drift coverage is desired: `python tools/run_receiver_vs_gnuradio.py docs/receiver_vs_gnuradio_matrix.json --output-json docs/receiver_vs_gnuradio_results.json --markdown docs/receiver_vs_gnuradio_report.md` (use `--captures <name ...>` to shard long recordings across jobs).
       *Example CI batches:*  
       `python tools/run_receiver_vs_gnuradio.py docs/receiver_vs_gnuradio_matrix.json --captures sf7_bw125_cr1_short sf9_bw125_cr3_short --output-json build/rvsg_batch1.json`  
       `python tools/run_receiver_vs_gnuradio.py docs/receiver_vs_gnuradio_matrix.json --captures sf5_bw125_cr1_full sf5_bw125_cr2_full --output-json build/rvsg_batch2.json`  
       `python tools/run_receiver_vs_gnuradio_matrix.json --captures sf7_bw500_cr2_implicithdr_nocrc sf11_bw125_cr1_short sf12_bw125_cr1_short --output-json build/rvsg_batch3.json`
    7. Upload `build/stage5_validation_summary.md` generated via:  
       `python3 tools/summarise_validation.py --ber build/stage5_ber_results.json --interop build/stage5_interop_results.json --soak build/stage5_live_soak_sf7_metrics.json --receiver-compare docs/receiver_vs_gnuradio_results.json --output build/stage5_validation_summary.md`
- [ ] Consider integrating soak/leak tests with the OTA replay to surface scheduling or latency issues observed in the field.
- [x] Added `tools/run_receiver_vs_gnuradio_ci.sh` to shard the matrix into `mid|low|high` capture batches. CI jobs can now run `tools/run_receiver_vs_gnuradio_ci.sh --batch <name> --merge` followed by `python3 tools/summarise_validation.py ... --receiver-compare build/receiver_vs_gnuradio_<name>.json` to gate wall-time/RAM/CPU regressions per batch.
- [x] When running the shard helper in automation, export `MC_RUNS=2` so each capture/profile is exercised twice with different seeds; the merged JSON retains `mc_iteration`/`mc_seed`, allowing `tools/summarise_validation.py --receiver-compare docs/receiver_vs_gnuradio_results.json` to flag regressions in mean/variance instead of a single timing sample.
