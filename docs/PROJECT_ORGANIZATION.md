# Project Organization

This page summarizes the streamlined layout of the repository. Directories under `external/` remain untouched so upstream code stays pristine.

## Top-Level Highlights

- `python_modules/` – shared helpers (e.g., `lora_decode_utils.py`).
- `config/` – runtime configuration (`requirements.txt`, `pytest.ini`).

## Directories by Purpose

| Directory | Purpose |
|-----------|---------|
| `config/` | Python requirements and pytest configuration. |
| `cpp_receiver/` | Modern C++ pipeline, including `decode_cli` and unit tests. |
| `docs/` | Documentation set (`GNU_RADIO_COMPAT.md`, validation reports, etc.). |
| `python_modules/` | Standalone Python modules used by the GNU Radio tooling (`lora_decode_utils.py`). |
| `results/` | Generated artefacts; long-running reports live in `results/archive/`. |
| `scripts/` | Supported CLIs (`lora_cli.py`, `sdr_lora_cli.py`, `sdr_lora_batch_decode.py`, `sdr_lora_offline_decode.py`). |
| `tests/` | Automated tests that validate the C++ receiver against GNU Radio; manual utilities live in `tests/manual/`. |
| `tools/` | Helper utilities such as vector generation and comparison scripts. |
| `vectors/`, `golden_vectors*/` | Captured and synthetic LoRa IQ vectors. |
| `external/` | Vendored dependencies (`gr-lora-sdr`); do not modify. |

## Recommended Entry Points

```bash
# Decode a single capture
python -m scripts.lora_cli decode vectors/example.cf32 --sf 7 --bw 125000 --fs 500000

# Batch-run the GNU Radio reference
python -m scripts.lora_cli batch --roots vectors --fast

# Execute the focused regression suite
pytest -q tests/test_gnu_radio_compat.py
```

## Notes

- Development-time artefacts (build directories, binaries, caches) are intentionally excluded from the repository.
- Deprecated demos and celebratory scripts have been removed; the universal CLI now exposes only the supported workflows.
- For GNU Radio parity details see `docs/GNU_RADIO_COMPAT.md`.
