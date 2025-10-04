# GNU Radio Compatibility Guide

This note summarises how the project keeps parity with GNU Radio's `gr-lora-sdr` implementation. All paths below are relative to the repository root.

## Reference Points

- **C++ receiver** (`cpp_receiver/`) – the in-tree implementation validated by our tests.
- **GNU Radio reference** (`external/sdr_lora`) – treated as ground truth and exercised via the scripts under `scripts/`.
- **GNU Radio ground truth** is gathered via
  ```bash
  python -m scripts.sdr_lora_cli decode <vector.cf32> --meta <vector.json>
  python -m scripts.sdr_lora_batch_decode --roots golden_vectors vectors
  ```

## Core Tests

| Test | Location | Description |
|------|----------|-------------|
| `tests/test_gnu_radio_compat.py` | `tests/` | End-to-end parity run that compares the C++ receiver against GNU Radio across curated vectors. |

Run the suite with:
```bash
pytest -q tests/test_gnu_radio_compat.py
```

## Useful Scripts

- `python -m scripts.lora_cli decode …` — Decode a single capture with the GNU Radio reference.
- `python -m scripts.lora_cli batch --roots vectors --out results/sdr_lora_batch.json` — Batch GNU Radio comparisons.

For low-level investigation, the GNU Radio project under `external/gr_lora_sdr` exposes `decode_offline_recording.py` and `export_tx_reference_vector.py`. These originals are kept untouched and should be run via the `conda run -n gr310` environment described in the project README.

## Reporting

Batch runs write their JSON summaries to `results/` (long-term artefacts live in `results/archive/`). The parity table used in documentation is regenerated via:
```bash
python tools/compare_all_receivers.py
```
which updates `results/receiver_comparison.json`.

## Notes

- The repository intentionally leaves `external/` unchanged so downstream updates from GNU Radio can be merged cleanly.
- When iterating on the C++ implementation, use the GNU Radio reference as the authoritative comparison point.
- All new vectors should include matching `.json` metadata so the GNU Radio scripts can infer parameters without extra flags.
