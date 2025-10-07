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

## Streaming diagnostics and GNU Radio parity tooling

This repository includes a focused workflow to compare the in-tree C++ streaming
receiver against the GNU Radio reference and gather automatic diagnostics for
header-stage failures. The goal is a fair, reproducible comparison and a set of
artifacts that make it easy to iterate on the header/payload algorithms.

### What was added (high-level)

- C++ streaming receiver improvements (header robustness and introspection):
  - Multi-offset header attempts around the fine timing estimate.
  - Optional CFO sweep during header decode with configurable range/step.
  - Strict header validation (CRC5 + field sanity) before accepting a header.
  - Automatic slice dumping (cf32) around preamble→header with configurable
    payload guard, and a JSON meta sidecar per slice.
  - Raw header bins are exported into the sidecar when header decode succeeds.
- Python tooling for batch comparison and diagnostics:
  - Run C++ vs GNU Radio across vector sets and summarize parity.
  - Re-run only failures to dump header slices with meta and CFO sweeping.
  - Attempt GNU Radio directly on the emitted slices to evaluate compatibility.
  - Aggregate slice meta files and print CFO/timing/ok-rate summaries.

### New/updated CLI flags (C++ `decode_cli`)

- `--streaming` – Use the chunked streaming receiver.
- `--chunk <N>` – Streaming chunk size (samples).
- `--payload-bytes` – Emit payload bytes as they decode.
- `--implicit-header` – Use implicit header (embedded profile) with:
  - `--payload-len <bytes>` – payload length in bytes
  - `--cr <1..4>` – coding rate
  - `--has-crc` / `--no-crc` – payload CRC16 presence
- `--hdr-cfo-sweep` – Enable small CFO sweep around the synchronizer CFO.
  - `--hdr-cfo-range <Hz>` – Sweep half-range (e.g., 200 means ±200 Hz).
  - `--hdr-cfo-step <Hz>` – Sweep step (e.g., 25 Hz).
- `--dump-header-iq <path>` – Dump cf32 slice from preamble through header and
  extra payload symbols (to aid external decoders like GNU Radio).
  - `--dump-header-iq-payload-syms <int>` – Number of extra payload symbols to include (default 64).
  - `--dump-header-iq-always` – Dump slice even if header decode fails.

Each `--dump-header-iq` slice produces a sibling JSON sidecar `<path>.meta.json`
with fields like:

- PHY: `sf`, `bw`, `fs`, `ldro_mode`, `sync_word`
- Header fields: `cr`, `has_crc`, `impl_header`, `payload_len`
- Synchronizer/decoder: `cfo_used_hz`, `p_ofs_est`, `cand_offset_samples`, `attempts`
- Slice indices: `slice_start`, `slice_end` (absolute indices within the stream window)
- Demod diagnostics: `header_ok` (CRC5), `header_bins` (8 raw bins when available)

### Python tools (under `tools/`)

- `compare_streaming_compat.py` – Batch C++ streaming vs GNU Radio
  - Args:
    - `--vectors <dir>` – Root of cf32+json vectors to process
    - `--limit <N>` – Limit number of vectors
    - `--output <path>` – Write JSON report with per-vector results
    - `--json-fallback` – Use sidecar payload_hex as expected when GNU Radio isn’t available
    - `--hdr-cfo-sweep` / `--hdr-cfo-range` / `--hdr-cfo-step` – Enable/param CFO sweep in C++
    - `--dump-header-iq-dir <dir>` – Emit header slices per vector into this directory
    - `--dump-header-iq-payload-syms <int>` – Extra payload syms for slices (forwarded)
    - `--dump-header-iq-always` – Dump slices even on header failures (forwarded)

- `dump_header_slices_for_failures.py` – Re-run only failed cases and dump slices
  - Args:
    - `--results <json>` – Results file from a prior comparison
    - `--prefix <path-substr>` – Restrict to vectors whose path starts with prefix
    - `--limit <N>` – Max failures to re-run
    - `--out-dir <dir>` – Where to save cf32 slices and meta sidecars
    - `--hdr-cfo-range <Hz>` / `--hdr-cfo-step <Hz>` – CFO sweep params
    - `--slice-payload-syms <int>` – Extra payload symbols for slice
    - `--slice-always` – Dump even on header failures

- `compare_header_slices_with_gnur.py` – Run GNU Radio on emitted slices
  - Prefers `<slice>.meta.json` to retrieve `sf/bw/fs/cr/ldro/impl/crc/sync_word`.
  - If absent, attempts to parse from original vector JSON or slice filename.
  - Prints a compact status line per slice (success/failed/timeout); on success,
    it includes the first lines from the GNU Radio decoder output.

- `analyze_header_slice_meta.py` – Summarize diagnostics from meta sidecars
  - Prints: total files, header_ok ratio, CFO used min/max/mean, histogram of candidate offsets, and quick per-(sf,bw) stats.
  - Optional `--csv` to export a per-file table for deeper analysis.

### Typical workflows

1) Full batch with CFO sweep and slice dumps
```bash
python -m tools.compare_streaming_compat \
  --vectors golden_vectors/new_batch \
  --hdr-cfo-sweep --hdr-cfo-range 200 --hdr-cfo-step 25 \
  --dump-header-iq-dir results/hdr_slices \
  --dump-header-iq-payload-syms 128 \
  --json-fallback \
  --output results/streaming_compat_results_cfo200.json
```

2) Re-run failures with diagnostics
```bash
python -m tools.dump_header_slices_for_failures \
  --results results/streaming_compat_results_cfo200.json \
  --prefix golden_vectors/new_batch \
  --limit 50 \
  --out-dir results/hdr_slices \
  --hdr-cfo-range 200 --hdr-cfo-step 25 \
  --slice-payload-syms 128 --slice-always
```

3) Try GNU Radio on the slices
```bash
python -m tools.compare_header_slices_with_gnur --dir results/hdr_slices --limit 50
```

4) Aggregate sidecar diagnostics
```bash
python -m tools.analyze_header_slice_meta --dir results/hdr_slices
```

### Notes on current status

- With CFO sweep (±200 Hz, 25 Hz step) the C++ streaming success is around ~66% over the provided new_batch set; remaining failures are concentrated at low SNR (e.g., SF12/BW125k/CR3).
- Header slice parity with GNU Radio on our emitted slices still fails in most cases; adding more preamble/payload guard and accurate meta sidecars is implemented and available for further experiments.
- The sidecars now capture raw header bins, CFO used, timing offsets, and candidate offset to guide algorithmic tweaks.

