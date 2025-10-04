# GNU Radio Compatibility Guide

This note summarises how the project keeps parity with GNU Radio's `gr-lora-sdr` implementation. All paths below are relative to the repository root.

## Reference Points

- **Python receiver** (`receiver/` + `complete_lora_receiver.py`) is treated as the reference implementation. It reuses GNU Radio's encoding logic through `external/sdr_lora` whenever possible.
- **C++ receiver** (`cpp_receiver/`) mirrors the Python pipeline stage-by-stage and is validated against the same vectors.
- **GNU Radio ground truth** is gathered via
  ```bash
  python -m scripts.sdr_lora_cli decode <vector.cf32> --meta <vector.json>
  python -m scripts.sdr_lora_batch_decode --roots golden_vectors vectors
  ```

## Core Tests

| Test | Location | Description |
|------|----------|-------------|
| `tests/test_gnu_radio_compat.py` | `tests/` | End-to-end parity run that compares the Python receiver and GNU Radio outputs across curated vectors. |
| `tests/test_original_no_heuristic.py` | `tests/` | Regression check that exercises the historical Python decoder without heuristics. |
| `tests/test_v3_at_original_position.py` | `tests/` | Confirms deterministic behaviour around legacy vector offsets. |

Run the suite with:
```bash
pytest -q tests/test_gnu_radio_compat.py
```

## Useful Scripts

- `python -m scripts.lora_cli decode …` — Decode a single capture with the Python receiver.
- `python -m scripts.lora_cli batch --roots vectors --out results/sdr_lora_batch.json` — Batch GNU Radio comparisons.
- `python -m scripts.lora_cli test --quick-test` — Focused regression suite.

For low-level investigation, the GNU Radio project under `external/gr_lora_sdr` exposes `decode_offline_recording.py` and `export_tx_reference_vector.py`. These originals are kept untouched and should be run via the `conda run -n gr310` environment described in the project README.

## Reporting

Batch runs write their JSON summaries to `results/` (long-term artefacts live in `results/archive/`). The parity table used in documentation is regenerated via:
```bash
python tools/compare_all_receivers.py
```
which updates `results/receiver_comparison.json`.

## Notes

- The repository intentionally leaves `external/` unchanged so downstream updates from GNU Radio can be merged cleanly.
- When the C++ implementation lags behind, the Python receiver remains the reference used by automated tooling.
- All new vectors should include matching `.json` metadata so the GNU Radio scripts can infer parameters without extra flags.
