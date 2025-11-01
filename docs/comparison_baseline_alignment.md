# Baseline Alignment Notes

These notes capture the initial feature parity and build requirements for the in-tree C++ LoRa receiver (`cpp_receiver`) and the vendored GNU Radio reference (`external/gr_lora_sdr`). They establish the configuration matrix, tooling entry points, and environmental prerequisites so subsequent comparison automation can assume a common baseline.

## Repository Revisions (2025-10-21 Snapshot)

| Component | Path | Source Commit/Tag | Notes |
|-----------|------|-------------------|-------|
| LoRa Lite PHY (C++ stack + tooling) | `.` | `332eeb38eaaea1f8c893f3ae7e6c8aa66c5a4baf` | Main repository checked out locally. |
| gr-lora_sdr reference | `external/gr_lora_sdr` | `master` (vendored snapshot, see `LOCAL_CHANGES.md`) | Upstream GNU Radio 0.5.8 sources with relative-path tweaks for local vector generation. |
| liquid-dsp (optional resampler) | `external/liquid-dsp` | Vendored | Only required when building with `-DLORA_ENABLE_LIQUID_DSP=ON`. |

Record full commit SHAs and any local patches when preparing experiment runs. Capture compiler versions (`cmake --version`, `ninja --version`, `gcc --version`) and hardware details (CPU SKU, RAM) in the experiment log.

## Vector Validation Tiers *(2025‑10‑21 update)*

| Tier | Location | Notes | Decoder Outcome |
|------|----------|-------|-----------------|
| “Easy” | `vectors/easy_set/tx_sf{7,8,9,10}_…` | SF7–SF10, BW 125 kHz, explicit header, CRC on, Fs=BW | C++ + GNU Radio → 100 % success, payloads match metadata |
| Challenging | `vectors/easy_set/tx_sf11_bw250000_cr2_crc1_impl0_ldro1_pay9` | Wider BW / higher SF | Both decoders succeed, payload match |
| Challenging | `vectors/easy_set/tx_sf12_bw125000_cr1_crc1_impl1_ldro2_pay9` | Implicit header, LDRO auto | C++ currently overshoots payload, GNURadio fails to acquire frame |
| Challenging | `vectors/easy_set/tx_sf9_bw125000_cr4_crc1_impl0_ldro0_pay9` | CR=4 with CFO≈50 Hz & SNR 10 dB | GNURadio OK; C++ returns wrong payload (CRC fails) |
| Challenging | `vectors/easy_set/tx_sf8_bw125000_cr2_crc1_impl1_ldro0_pay9` | Implicit header + impairments | Both decoders currently miss payload |

The “easy” tier acts as a sanity baseline (pass/fail), and the challenging tier highlights current gaps that need fixes in the C++ receiver (and more padding for GNU Radio).

When running comparative tests, ensure the `results/easy_set_manifest.json` manifest is included so regressions can track these tiers explicitly.

| Dimension | C++ Receiver (`cpp_receiver/include/receiver.hpp`) | GNU Radio (`external/gr_lora_sdr`) | Notes |
|-----------|----------------------------------------------------|------------------------------------|-------|
| Spreading factor | Integer 5–12 enforced in constructors | 5–12 (`scripts/decode_offline_recording.py` CLI, GR blocks) | SF5/SF6 work in C++ including brute-force header fallback. GR notes SF5/SF6 incompatible with SX126x hardware. |
| Bandwidth | Any positive integer; must divide sample rate exactly (asserted in `FrameSynchronizer`) | Typical LoRa set {125 kHz, 250 kHz, 500 kHz}; CLI accepts arbitrary values but LoRa semantics assumed | Maintain shared matrix of BW values used in regressions. |
| Sample rate | Integer multiple of BW. Optional resampling when CFO/SR estimated ratio deviates (`Receiver::build_resampled_capture`). | User supplies sample rate; default inference fallback equals BW. On-air flowgraphs support arbitrary rates consistent with hardware. | Ensure vector manifests specify both Fs and BW for alignment. |
| Coding rate (CR) | 1–4. Implicit header path enforces range. Explicit header verifies CRC5 before use. | 0–4 options exposed; practical LoRa uses 1–4 (CR=0 = uncoded). | Treat CR0 as experimental; include if vectors exist. |
| Header mode | Explicit (default) or implicit via CLI flags. | Explicit / implicit toggle via GR flowgraphs and CLI. | Align CLI flags per matrix row. |
| Payload length | 0–255 (validated in header decode). | 1–255 (script default 255). | Document actual payload length per vector. |
| LDRO | Manual flag plus auto enable heuristic (`auto_ldro`). | CLI exposes `--ldro-mode` (0=off,1=on,2=auto). | Align semantics when generating vectors. |
| Sync word | 8-bit (two nibbles). Skip check possible via `--skip-syncword`. | CLI `--sync-word` (list, default 0x12). | For multi-network evaluation, capture sync word and internal network ID mapping. |
| Sample-rate tracking | Optional manual correction (`--ppm-offset`) and threshold-based resampling; caching of SR hints in streaming path. | Frame sync block estimates and applies SFO. Offline script assumes GR internal correction. | Plan to log sample_rate_ratio and resampling events. |
| CFO handling | Frame sync estimates coarse CFO; header CFO sweep (`--hdr-cfo-*`) and payload retries include additional offsets. | Frame sync block estimates STO/CFO; internal loops handle residual offsets. | Document CFO sweep configuration alongside vectors. |
| Streaming support | `StreamingReceiver` (`cpp_receiver/src/streaming_receiver.cpp`), chunked processing with SOP/EOP instrumentation and socket harness. | GNU Radio flowgraphs for real-time streaming; TCP socket interface available via GNU Radio scheduler. | Mapping required between harness scripts. |

## Tooling Entry Points

### C++ Receiver

| Tool | Path | Mode | Key Flags |
|------|------|------|-----------|
| Batch CLI | `cpp_receiver/tools/decode_cli.cpp` | One-shot (`Receiver`) or streaming (`StreamingReceiver`) | `--sf`, `--bw`, `--fs`, `--ldro`, `--streaming`, `--chunk`, `--implicit-header`, `--hdr-cfo-sweep`, `--ppm-offset`, `--resample-threshold-ppm`, `--dump-header-iq`. |
| Streaming harness | `cpp_receiver/tools/streaming_harness.cpp` | Replays chunks from file/socket, exposes instrumentation CSV | Needs extension for structured logging (see §6 of master plan). |
| Regression tooling | `tools/run_channel_regressions.py`, `tools/compare_streaming_compat.py` | Batch compare vs GNU Radio reference | Already supports header CFO sweep and slice dumping. |

### GNU Radio Reference (gr-lora_sdr)

| Tool | Path | Mode | Key Flags |
|------|------|------|-----------|
| Offline decoder | `external/gr_lora_sdr/scripts/decode_offline_recording.py` | Batch file decode | `--sf`, `--bw`, `--samp-rate`, `--cr`, `--impl-header`, `--ldro-mode`, `--sync-word`, `--dump-json`. |
| Vector generator | `external/gr_lora_sdr/scripts/export_tx_reference_vector.py` | Batch vector synthesis with impairments | Supports AWGN, Doppler, Rayleigh, CFO injection. |
| Simulation flowgraph | `external/gr_lora_sdr/examples/tx_rx_simulation.py` | Streaming (file/TCP) | Accepts parameter overrides for SF/BW/CR, supports channel models. |
| GNU Radio Companion blocks | `external/gr_lora_sdr/examples/*.grc` | Live SDR or simulation pipelines | Ensure consistent parameter templates before headless runs. |

## Build & Environment Summary

### C++ Receiver (`cpp_receiver/`)

1. Configure build directory:
   ```bash
   cmake -S cpp_receiver -B cpp_receiver/build -GNinja \
         -DCMAKE_BUILD_TYPE=Release \
         -DLORA_ENABLE_LIQUID_DSP=ON   # optional, requires `external/liquid-dsp`
   ```
2. Build targets:
   ```bash
   cmake --build cpp_receiver/build --target decode_cli streaming_harness run_stage_tests
   ```
3. Tests:
   ```bash
   ctest --test-dir cpp_receiver/build          # C++ stage tests
   pytest -q tests/test_gnu_radio_compat.py     # Python parity tests
   ```

Prerequisites: `cmake>=3.20`, `ninja`, `gcc`/`clang` (C++20), optionally `liquid-dsp` headers if enabling resampler acceleration.

### GNU Radio Reference (`external/gr_lora_sdr`)

1. Activate GNU Radio toolchain (recommended Conda env):
   ```bash
   conda env create -f environment.yml          # once
   conda activate gr310
   ```
2. Configure + build:
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX=$CONDA_PREFIX
   cmake --build build --target install
   ```
3. Optional verification:
   ```bash
   python examples/tx_rx_functionality_check.py   # loopback smoke test
   python scripts/decode_offline_recording.py --help
   ```

Prerequisites: GNU Radio 3.10, VOLK, UHD (for hardware tests), `pybind11`, and Conda toolchain recommended for dependency management.

## Configuration Normalisation Checklist

1. **Matrix definition**: enumerate combinations for SF ∈ {5..12}, BW ∈ {125k, 250k, 500k}, CR ∈ {4/5, 4/6, 4/7, 4/8}, header mode ∈ {explicit, implicit}, LDRO ∈ {off, auto, forced}. Extend matrix when evaluating CR=4/5 alias vs CR index (1–4).
2. **Flag mapping**: maintain a CSV mapping experimental row → CLI flags for:
   - `decode_cli`
   - `streaming_harness`
   - `scripts/decode_offline_recording.py`
   - `examples/tx_rx_simulation.py` (parameter overrides)
3. **Sample rate policy**: choose canonical Fs per BW (default 2× or 4× oversampling). Document fallback for captures lacking metadata.
4. **Metadata manifest**: ensure every IQ vector has a sidecar JSON with keys `{sf, bw, sample_rate_hz, cr, ldro, implicit_header, payload_len, has_crc, sync_word, impairments}`. Future tooling will ingest this manifest.
5. **Logging alignment**: plan to capture sample-rate ratio (`Receiver::DecodeResult.sample_rate_ratio_used`) and GR frame sync stats once instrumentation is added.

These baseline notes should be expanded with precise CLI templates and environment capture before executing large comparison batches.
