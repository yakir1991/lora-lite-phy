# Test Vector Corpus

This note inventories the LoRa IQ vector suites already in-tree and outlines how future automation should describe them via a unified manifest. It complements the baseline alignment notes and provides the raw material for the comparison campaign.

## Repository Layout (2025-10-21)

| Location | Contents | Notes |
|----------|----------|-------|
| `golden_vectors/` | Canonical regression suites grouped by scenario (`custom/`, `extended_batch/`, `extended_batch_impl/`, etc.). Each capture ships with a `.json` sidecar containing PHY configuration, payload hex, and optional impairment fields such as `snr_db`. | Used by parity/unit tests and smoke checks. |
| `golden_vectors_demo/` | Minimal demo capture + metadata. | Handy for docs and quick CLI tests. |
| `vectors/` | Working directory for generated or experimental vectors. Populated by `tools/run_channel_regressions.py` and ad‑hoc scripts. Subdirectories include:<br>• `custom_*` – handcrafted captures by SF.<br>• `synthetic_batch/` – SNR sweep generated via GNU Radio exporter.<br>• `synthetic_impairments{,_air,_clean}/` – scenarios combining AWGN, CFO, multipath.<br>• `sweep_*_{clean,air}/` – parameter sweeps (CFO, Rayleigh, sampling PPM, etc.).<br>• `new_cases_{clean,air}/` – curated “hard” cases for debugging.<br>• `air_vectors/`, `clean_vectors/`, `custom_air*` – captures post-channel simulation versus clean references. | Most directories currently contain placeholders until `tools/run_channel_regressions.py --regen` is executed (requires GNU Radio env). |
| `results/reference_case/` | Case studies for known failures. | Useful for reproducing edge cases and linking additional diagnostics. |

When regenerating vectors, remember to enable GNU Radio bindings first:
```bash
conda activate gr310
```

## Metadata Sidecars

Each `.cf32` IQ file is coupled with a JSON sidecar. Common fields observed across the repository:

| Key | Meaning |
|-----|---------|
| `sf` | Spreading factor (integer 5–12). |
| `bw` | Bandwidth in Hz. |
| `samp_rate` / `sample_rate` | Sample rate in Hz (aliasing present across generators; manifest should normalise to `sample_rate_hz`). |
| `cr` | Coding-rate index (1 → 4/5, …, 4 → 4/8). |
| `crc` | Boolean payload CRC flag. |
| `impl_header` / `implicit_header` | Boolean header mode (explicit vs. implicit). Both spellings appear; consume both. |
| `ldro_mode` | LoRa LDRO configuration: 0=off, 1=forced on, 2=auto. |
| `payload_len` | Byte length of payload. |
| `payload_hex` | Hex-encoded payload ground truth. |
| `filename` | Original IQ filename within the directory. |
| `snr_db` | Present when AWGN was applied. |
| `generator` | Provenance tag (`custom_gnuradio`, `tools.add_awgn`, etc.). |
| `channel` | Nested dict describing applied impairments (sampling offset, CFO, Rayleigh parameters, clipping, quantisation). |

Manifest utilities must tolerate the optional presence of impairment-specific keys (e.g., `cfo_hz`, `sampling_offset_ppm`, `multipath_taps`) either at top-level or within `channel`.

## Vector Generation Tooling

Existing scripts and their roles:

- `tools/run_channel_regressions.py`  
  Orchestrates large sweeps by calling the GNU Radio exporter (`external/gr_lora_sdr/scripts/export_tx_reference_vector.py`) with channel impairments enabled (`--channel-sim`, `--air-output`). Produces paired clean/air captures and runs parity checks.

- `tools/generate_noisy_explicit_set.py` + `tools/generate_random_vectors.py` + `tools/add_awgn.py`  
  Compose random payloads across SF/BW/CR, emits clean vectors, then injects AWGN over a target SNR range.

- `external/gr_lora_sdr/scripts/export_tx_reference_vector.py`  
  Ground-truth generator supporting single-frame and batched exports with rich channel models: CFO (`--cfo-hz`, `--cfo-drift-hz`), sampling offsets, Rayleigh fading (`--rayleigh-fading`, `--fading-doppler-hz`), multipath taps, IQ impairments, clipping/quantisation, and AWGN (`--snr-db`). Also supports `--air-output` to split impaired vs. clean directories.

- `external/gr_lora_sdr/examples/tx_rx_simulation.py`  
  Streaming simulator for socket/file pipelines replicating the same parameter set; future harness work will invoke it for live-mode comparisons.

## Manifest Proposal

Create a single machine-readable manifest (JSON or CSV) capturing every vector used in benchmarking. Recommended schema:

```json
{
  "path": "vectors/synthetic_batch/tx_sf8_bw250000_cr4_crc1_impl0_ldro0_pay21.cf32",
  "group": "synthetic_batch",
  "source": "gr_lora_sdr/export_tx_reference_vector",
  "sf": 8,
  "bandwidth_hz": 250000,
  "sample_rate_hz": 500000,
  "coding_rate": 4,
  "header_mode": "explicit",
  "ldro_mode": 0,
  "payload_len": 21,
  "payload_hex": "b73b38a1df9244b33fd8e3189d42f74f356b91c474",
  "snr_db": 1.1,
  "impairments": {
    "cfo_hz": 0.0,
    "cfo_drift_hz": 0.0,
    "sampling_offset_ppm": 0.0,
    "rayleigh_fading": false,
    "multipath": null,
    "iq_imbalance_db": 0.0,
    "iq_phase_deg": 0.0,
    "quantize_bits": null,
    "clip": null
  },
  "notes": "Generated via channel regression sweep 2025-10-21"
}
```

Guidelines:

- **Normalise field names**: convert `impl_header`/`implicit_header` into `header_mode`, ensure numeric fields are stored as numbers, and lift nested `channel` dictionary into the `impairments` map.
- **Record provenance**: include commit SHA, generator command, and run identifier (timestamp or tag) either in the manifest root or per-entry `notes`.
- **Support clean vs. air pairing**: add optional fields `clean_path` / `air_path` when vectors exist in both forms (as produced by `--air-output`).
- **Keep manifest reproducible**: regenerate via a deterministic script (e.g., `tools/build_vector_manifest.py`) that scans the directories and validates JSON schema.

Maintaining this manifest will allow `tools/run_channel_regressions.py` (and future automation) to select scenario subsets, schedule comparison runs, and generate aggregate reports reproducibly.
