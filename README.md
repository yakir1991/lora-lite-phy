# LoRa Lite PHY

LoRa Lite PHY bundles two receivers that share the same vector corpus and test harnesses:

- **Python reference receiver** – wraps GNU Radio's `gr-lora-sdr` implementation (`scripts/sdr_lora_cli.py`) and is used for day-to-day decoding, batch comparisons, and automated tests.
- **C++ receiver** – a clean-room reimplementation that mirrors the Python pipeline stage by stage (`cpp_receiver/`). It ships with a standalone CLI (`decode_cli`) and unit tests so that new optimisations can be validated against the Python reference.

The project keeps the original GNU Radio scripts under `external/` untouched; everything else lives in this repository so it is easy to integrate with CI, testing, and tooling.

## Quick Start

### Environment
```bash
git clone https://github.com/yakir1991/lora-lite-phy.git
cd lora-lite-phy
python -m venv .venv
source .venv/bin/activate  # Windows: .venv\Scripts\activate
pip install -r requirements.txt
```

### Decode a capture (Python/GNU Radio reference)
```bash
python -m scripts.lora_cli decode vectors/gnuradio_sf7_cr45_crc.bin --verbose
```
- `--meta` can be passed if the `.json` sidecar is stored elsewhere.
- `--sf`, `--bw`, and `--fs` act as fallbacks when metadata is missing.

### Batch comparison against GNU Radio
```bash
python -m scripts.lora_cli batch --roots vectors golden_vectors --out results/sdr_lora_batch.json --fast
```
The batch report lists which captures matched the GNU Radio output and stores the full JSON summary under `results/`.

### Test suites
```bash
pytest -q tests/test_gnu_radio_compat.py          # Python vs GNU Radio parity
python scripts/lora_test_suite.py --quick-test    # Smoke tests across curated vectors
```

### Build and run the C++ receiver (optional)
```bash
cmake -S cpp_receiver -B cpp_receiver/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp_receiver/build --target decode_cli
cpp_receiver/build/decode_cli --sf 7 --bw 125000 --fs 500000 --ldro 0 golden_vectors_demo/tx_sf7_bw125000_cr2_crc1_impl0_ldro0_pay11.cf32
```
The CLI prints sync/header status and the decoded payload in hex. Additional diagnostics are available with `--debug`.

## Reference material

| Location | Contents |
|----------|----------|
| `golden_vectors*/*` | Canonical IQ captures plus JSON sidecars used in regression tests. |
| `results/receiver_comparison.json` | Latest comparison table between Python, C++, and GNU Radio outputs. |
| `docs/GNU_RADIO_COMPAT.md` | Summary of the parity workflow and reporting tools. |
| `docs/COMPLETE_SYSTEM_DOCUMENTATION.md` | Detailed design notes for the receiver pipeline. |

## Project layout

```
lora-lite-phy/
├── analysis/              # Research scripts (position optimisation, hybrid demodulation, etc.)
├── benchmarks/            # Performance studies and batch experiments
├── cpp_receiver/          # C++ implementation + decode_cli
├── debug/                 # Local debugging helpers (manual experiments)
├── docs/                  # Project documentation, validation reports, compatibility notes
├── legacy_receivers/      # Archived Python receiver variants retained for reference
├── receiver/              # Modular Python receiver package (used by scripts/sdr_lora_cli)
├── results/               # Batch reports and truth tables (`archive/` holds historical JSON)
├── scripts/               # CLI entry points (lora_cli, sdr_lora_cli, batch decode, test suite)
├── tests/                 # Automated tests (unit + integration)
├── tools/                 # Vector generation / comparison helpers
├── vectors/               # Captured & synthetic LoRa IQ files used in testing
└── external/              # Vendored gr-lora-sdr; left untouched
```

## Housekeeping files

- `requirements.txt` – Python dependencies for the tooling.
- `pytest.ini` – Pytest configuration used by the suite.
- `lora_cli.py` – backward-compat entry point kept for historical scripts (delegates to `scripts/lora_cli`).
- `complete_lora_receiver.py` – wrapper that loads the modular Python receiver package.
- `.gitignore`, `.gitmodules` – Git metadata.

## Notes

- `external/` mirrors upstream projects and must not be modified; changes go in the top-level code.
- When adding new IQ captures, include the matching `.json` sidecar so the batch tools can infer parameters automatically.
- The Python CLI is the canonical reference; the C++ implementation should always be validated against it before committing major changes.
