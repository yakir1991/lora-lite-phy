# Project File Overview

This document lists the major files and directories in the repository (excluding everything under `external/`) with a short description of their purpose. Refer to `docs/PROJECT_ORGANIZATION.md` for a directory-level map; this page focuses on individual components.

## Shared Python modules (`python_modules/`)
- `lora_decode_utils.py` – Whitening, deinterleaving, and CRC helpers reused by the GNU Radio tooling (`external/sdr_lora`).

## Configuration (`config/`)
- `config/requirements.txt` – Pip dependencies needed by the Python tooling.
- `pytest.ini` – Pytest configuration (markers, log-levels, test discovery rules).

## Receivers
- `cpp_receiver/src/*.cpp`, `cpp_receiver/include/*.hpp` – C++ implementation and headers; `cpp_receiver/tools/decode_cli.cpp` builds the standalone decoder.
- `external/sdr_lora` – Vendored GNU Radio reference kept pristine for parity checks (accessed via `scripts/sdr_lora_cli.py`).

## Testing
- `scripts/lora_cli.py` – User-facing universal CLI (`python -m scripts.lora_cli`).
- `scripts/sdr_lora_cli.py` / `scripts/sdr_lora_batch_decode.py` / `scripts/sdr_lora_offline_decode.py` – GNU Radio reference wrappers.
- `tests/test_gnu_radio_compat.py` – pytest harness comparing the C++ receiver with GNU Radio.
- `tests/manual/test_gnu_radio_compat_cli.py` – CLI wrapper around the same parity check.

## Tooling (`tools/`)
- `generate_random_vectors.py`, `generate_gnur_truth.py`, `generate_python_truth.py` – Scripts to create synthetic vectors and reference payloads.
- `compare_all_receivers.py`, `compare_runtime.py` – Summaries comparing the GNU Radio reference and the C++ receiver.

## Documentation (`docs/`)
- `GNU_RADIO_COMPAT.md` – Testing workflow against GNU Radio.
- `PROJECT_ORGANIZATION.md` – Directory-level map.
- `RECEIVER_PARITY_STATUS.md` – Current parity summary across receivers.
- `PAYLOAD_REFERENCE.md` – Payload decoding notes and truth tables.
- `lorawebinar-lora-1.pdf` – Background slides kept for context.

## Data & results
- `vectors/`, `golden_vectors*/`, `golden_vectors_demo*/` – IQ captures (real and synthetic) plus metadata sidecars.
- `results/receiver_comparison.json` – Latest comparison table between the C++ receiver and GNU Radio; archived reports live in `results/archive/` when generated.

## Miscellaneous
- `tools/` – Utilities exposed via the standard CLIs as described in the main README.
- Hidden files in the repository root (`.gitignore`, `.gitmodules`) are Git metadata retained for development.

For everything else (e.g., new captures, scripts, or analyses) follow the patterns above when adding files so they remain discoverable.
