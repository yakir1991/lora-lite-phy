# Project Organization

This page summarizes the streamlined layout of the repository. Directories under `external/` remain untouched so upstream code stays pristine.

## Top-Level Highlights

- `complete_lora_receiver.py` – production Python entry point.
- `lora_cli.py` – backward compatible shim that forwards to `scripts.lora_cli`.
- `lora_decode_utils.py` – whitening/CRC helpers shared by receivers.

## Directories by Purpose

| Directory | Purpose |
|-----------|---------|
| `analysis/` | Research utilities and deep-dive experiments used during algorithm development. |
| `benchmarks/` | Batch and performance experiments. |
| `cpp_receiver/` | Modern C++ pipeline, including `decode_cli` and unit tests. |
| `debug/` | Ad-hoc debugging helpers kept out of the production path. |
| `docs/` | Documentation set (`GNU_RADIO_COMPAT.md`, validation reports, etc.). |
| `include/`, `src/` | Shared headers and supplementary C++ source files. |
| `legacy_receivers/` | Archived Python receiver variants retained for historical reference. |
| `receiver/` | Modular Python receiver package used by the production CLI. |
| `results/` | Generated artefacts; long-running reports live in `results/archive/`. |
| `scripts/` | Supported CLIs (`lora_cli.py`, `sdr_lora_cli.py`, `sdr_lora_batch_decode.py`, `sdr_lora_offline_decode.py`, `lora_test_suite.py`). |
| `tests/` | Automated tests (Python and C++). Manual experiments live in `tests/manual/`. |
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
python -m scripts.lora_cli test --quick-test
```

## Notes

- Development-time artefacts (build directories, binaries, caches) are intentionally excluded from the repository.
- Deprecated demos and celebratory scripts have been removed; the universal CLI now exposes only the supported workflows.
- For GNU Radio parity details see `docs/GNU_RADIO_COMPAT.md`.
