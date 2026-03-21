# Validation Playbook – LoRa PHY (Simulation-First)

This playbook captures the recurring validation tasks for the standalone LoRa PHY while hardware is still pending. All commands assume execution from the repo root unless noted otherwise.

## 1. BER/PER Sweeps

### Quick Regression Sweep
```
ctest -R host_sim_ber_sweep --output-on-failure
```
* Uses `docs/stage5_ber_matrix.json`.
* Output JSON: `build/stage5_ber_results.json`.
* Compare against baseline (fails CTest automatically):
```
ctest -R host_sim_ber_compare --output-on-failure
```

### Nightly / Extended Sweep
```
ctest -R host_sim_ber_sweep_nightly --output-on-failure
```
* Matrix: `docs/stage5_ber_matrix_nightly.json`.
* Output JSON: `build/stage5_ber_results_nightly.json`.
* Optional CSV:
```
python3 tools/run_ber_sweep.py \
    docs/reference_stage_manifest.json \
    gr_lora_sdr/data/generated \
    build/stage5_ber_results_nightly.json \
    --matrix docs/stage5_ber_matrix_nightly.json \
    --summary-dir build/stage5_ber_summaries_nightly \
    --lora-replay build/host_sim/lora_replay \
    --csv build/stage5_ber_results_nightly.csv
```

## 2. GNU Radio Interoperability

### Flow
1. Ensure `gr310` conda env includes GNU Radio + gr-lora_sdr deps.
2. Run the scripted compare:
```
ctest -R host_sim_interop_compare --output-on-failure
```
* Internally executes `tools/run_interop_compare.py`.
* Output JSON: `build/stage5_interop_results.json`.
* Additional payload dumps stored under `build/stage5_interop/<capture_name>/`.

### Manual Invocation
```
python3 tools/run_interop_compare.py \
    docs/reference_stage_manifest.json \
    gr_lora_sdr/data/generated \
    build/stage5_interop_results.json \
    --captures tx_rx_sf7_bw125000_cr1_snrm5p0_short.cf32 \
    --work-dir build/stage5_interop \
    --lora-replay build/host_sim/lora_replay \
    --gnuradio-env gr310
```

### SF/CR Regression Sweep
```
python3 tools/run_sfcr_regression.py \
    --matrix docs/receiver_vs_gnuradio_sweep_matrix.json \
    --result-json build/receiver_vs_gnuradio_sweep_results.json \
    --summary-md build/receiver_vs_gnuradio_sweep_summary.md \
    --report-md docs/receiver_vs_gnuradio_sweep_report.md
```
* Runs the full SF7–SF12, CR1–CR4, BW 125/250/500 kHz sweep via `run_receiver_vs_gnuradio.py` and then emits a compact Markdown summary highlighting payload mismatches per SF/CR bucket.
* Requires the synthetic captures under `gr_lora_sdr/data/generated` (regenerate with `python3 tools/generate_sim_captures.py` if needed).
* Use `--captures sweep_sf9_bw125k_cr1 sweep_sf9_bw125k_cr2` to focus on a subset, or `--skip-run` to rebuild the summary from an existing JSON without re-running either decoder.
* Outputs:
  * Raw comparison JSON: `build/receiver_vs_gnuradio_sweep_results.json` (default, pass `--result-json` to override).
  * Sweep timings + statuses table: `docs/receiver_vs_gnuradio_sweep_report.md` (set via `--report-md`).
  * Aggregated pass/fail view: `build/receiver_vs_gnuradio_sweep_summary.md` (set via `--summary-md`).

## 3. Live Streaming Soak

### Quick Loopback
```
ctest -R host_sim_live_loopback --output-on-failure
```

### Impairment Soak (Opt-in)
```
ctest -R host_sim_live_soak_sf7 --output-on-failure
ctest -R host_sim_live_soak_sf9 --output-on-failure
```
* Outputs metrics JSONs: `build/stage5_live_soak_sf7_metrics.json` and `build/stage5_live_soak_sf9_metrics.json`.
* To replay captures (or the simulated `.cf32` assets) outside of CTest:
```
build/host_sim/host_sim_live_soak \
    --mode capture \
    --capture docs/captures/session_x/iq/example.cf32 \
    --metadata docs/captures/session_x/iq/example.json \
    --json-out build/stage5_live_soak_custom.json \
    [--max-symbols 2048] [--impair-awgn-snr 8 --impair-cfo-ppm 5]
```
  - Synthetic stress remains the default (`--mode synthetic`) and accepts `--total-symbols`, `--samples-per-symbol`, and the same impairment flags (`--impair-*`) for MTBF sweeps.
* For nightly runs:
```
ctest -L host_sim_soak --output-on-failure
```
  - Runs both soak variants above so SF7/SF9 coverage remains active.

## 4. Summary Dashboards

After running the validations above, aggregate JSON outputs into `build/stage5_validation_summary.md` (generated via tooling below):
```
mkdir -p build
python3 tools/summarise_validation.py \
    --ber build/stage5_ber_results.json \
    --interop build/stage5_interop_results.json \
    --soak build/stage5_live_soak_sf7_metrics.json \
    --receiver-compare docs/receiver_vs_gnuradio_results.json \
    --output build/stage5_validation_summary.md
```
(re-run with `--soak build/stage5_live_soak_sf9_metrics.json` if you want a second report.)

The receiver comparison JSON now captures runtime, RAM, and CPU metrics for both stacks; the generated summary (`docs/receiver_vs_gnuradio_summary.md`) includes updated tables and graphs (`images/receiver_compare_{runtime,speedup,config,ram,cpu}.png`).

## 5. Cleanup / Re-run Tips
- Remove old summaries before new runs:
```
rm -rf build/stage5_* stage5_live_soak_*_metrics.json
```
- Rebuild binaries prior to testing:
```
cmake --build build -j
```

## 6. File Locations Overview
| Task                  | Output Path                                   |
| --------------------- | --------------------------------------------- |
| BER (quick)           | `build/stage5_ber_results.json`               |
| BER (nightly)         | `build/stage5_ber_results_nightly.json`       |
| BER CSV               | `build/stage5_ber_results_nightly.csv`        |
| Interop               | `build/stage5_interop_results.json`           |
| Soak Metrics          | `build/stage5_live_soak_sf7_metrics.json`, `build/stage5_live_soak_sf9_metrics.json` |
| Dashboard (generated) | `build/stage5_validation_summary.md`          |
| GNU Radio Comparison  | `docs/receiver_vs_gnuradio_results.json`, `docs/receiver_vs_gnuradio_report.md` |
