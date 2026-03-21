# Stage 5 – Hardware Bridging Dossier (Simulation-Ready)

Even though we are still operating in a pure simulation environment, the following checklist captures the artefacts and procedures required the moment SDR hardware becomes available. This document should travel with the first bring-up kit.

## 1. Software Staging

### Environments
- **Conda (gr310)** – GNU Radio + `gr-lora_sdr` modules (`conda run -n gr310 …`).
- **Standalone host build** – `cmake --build build` (produces `host_sim/lora_replay`, soak binaries, etc.).

### Baseline Scripts
| Purpose | Command | Notes |
| --- | --- | --- |
| BER sweeps (quick) | `ctest -R host_sim_ber_sweep --output-on-failure` | Outputs `build/stage5_ber_results.json`. |
| BER sweeps (nightly) | `ctest -R host_sim_ber_sweep_nightly --output-on-failure` | Wider SNR set. |
| Interop diff | `ctest -R host_sim_interop_compare --output-on-failure` | Generates `build/stage5_interop_results.json`. |
| Live soak | `ctest -L host_sim_soak --output-on-failure` | Streaming stress test. |

## 2. SDR Integration Plan

### Capture Playback (PC → SDR)
- Use `SoapyLiveAdapter` once hardware is connected. Example pseudo-code (to be expanded):
  ```cpp
  host_sim::LiveSymbolSource source(256);
  host_sim::SoapyLiveAdapter adapter(source, soapy_cfg);
  adapter.start();
  scheduler.run(source);
  ```
- Required configuration fields (`SoapyStreamConfig`): `args`, `sample_rate`, `bandwidth`, `frequency`, `gain`, `samples_per_symbol`.

### IQ Recording (SDR → PC)
- Prefer external tooling (`SoapySDRUtil`, GNU Radio) to capture `.cf32` while host software is not yet running on the device.
- Store raw captures under `hardware_captures/<date>/<scenario>.cf32` with matching metadata copy (JSON derived from reference).

## 3. Data & Artefact Checklist

| Item | Location | Notes |
| --- | --- | --- |
| Simulation baselines (BER/interp/soak) | `build/stage5_*.json` or `docs/stage5_ber_baseline.json` | Regenerate before hardware tests. |
| Validation playbook | `docs/validation_playbook.md` | Quick references for testing commands. |
| Metrics dashboard | `build/stage5_validation_summary.md` | Append new rows after each hardware campaign. |
| Soapy adapter source | `host_sim/include/host_sim/soapy_live_adapter.hpp` | Update once real device characteristics are known. |

## 4. When Hardware Arrives

1. **Connectivity check** – verify `SoapySDRUtil --find` sees the board.
2. **Loopback sanity** – run `host_sim_live_loopback` to ensure host vs. pipeline handshake still works.
3. **Device streaming** – enable `SoapyLiveAdapter` with conservative buffers; reuse the live soak pipeline to identify overruns.
4. **Capture comparisons** – re-run `tools/run_interop_compare.py` with new `.cf32` recordings to confirm the standalone decoder matches GNU Radio on the same data.

## 5. Outstanding TODO (Hardware-Only)
- RF chain calibration (frequency offset, gain table) – pending access to lab equipment.
- OTA BER sweeps – placeholder scripts should reuse the existing BER harness but backed by live captures.
- Automated artefact collection – set up pipelines to upload capture logs and updated metrics after each field session.

