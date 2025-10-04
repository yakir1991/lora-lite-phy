# LoRa Lite PHY

LoRa Lite PHY centres on a clean-room C++ LoRa receiver that is validated against
the upstream GNU Radio implementation vendored under `external/sdr_lora`.

- **C++ receiver (`cpp_receiver/`)** – standalone implementation with unit tests
  and a `decode_cli` binary for manual runs. This is the codebase you extend.
- **GNU Radio reference (`external/sdr_lora`)** – kept untouched and exercised
  through lightweight Python wrappers in `scripts/` for parity checks and batch
  decoding.

The GNU Radio sources remain pristine so they can be updated from upstream while
the in-tree C++ receiver evolves independently.

## Quick Start

### Environment
```bash
git clone https://github.com/yakir1991/lora-lite-phy.git
cd lora-lite-phy
python -m venv .venv
source .venv/bin/activate  # Windows: .venv\Scripts\activate
pip install -r config/requirements.txt
```

### Decode a capture (GNU Radio reference)
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

### Tests
```bash
pytest -q tests/test_gnu_radio_compat.py          # GNU Radio vs C++ parity
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
| `results/receiver_comparison.json` | Latest comparison table between the C++ receiver and the GNU Radio reference. |
| `docs/GNU_RADIO_COMPAT.md` | Summary of the parity workflow and reporting tools. |
| `docs/PROJECT_ORGANIZATION.md` | Directory-level overview of the repository structure. |

## Project layout

```
lora-lite-phy/
├── config/                # requirements.txt, pytest.ini
├── python_modules/        # Shared helpers (e.g., lora_decode_utils.py)
├── cpp_receiver/          # C++ implementation + decode_cli
├── docs/                  # Project documentation, validation reports, compatibility notes
├── results/               # Batch reports and truth tables (`archive/` holds historical JSON)
├── scripts/               # CLI entry points (lora_cli, sdr_lora_cli, batch decode)
├── tests/                 # Automated tests (GNU Radio vs C++ parity)
├── tools/                 # Vector generation / comparison helpers
├── vectors/               # Captured & synthetic LoRa IQ files used in testing
├── golden_vectors*/       # Published reference suites
├── golden_vectors_demo*/  # Demo vector sets used in documentation
└── external/              # Vendored gr-lora-sdr; left untouched
```

## Housekeeping files

- `config/requirements.txt` – Python dependencies for the tooling.
- `config/pytest.ini` – Pytest configuration used by the suite.
- `.gitignore`, `.gitmodules` – Git metadata left at the repository root.

## Notes

- `external/` mirrors upstream projects and must not be modified; changes go in the top-level code.
- When adding new IQ captures, include the matching `.json` sidecar so the batch tools can infer parameters automatically.
- Use `python -m scripts.lora_cli` to invoke the GNU Radio reference decoder when
  ground truth is required during development.
- Validate C++ changes against the GNU Radio reference before committing major
  updates.
