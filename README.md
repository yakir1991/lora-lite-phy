# LoRa Lite PHY — Utilities & Loopback

This repository hosts the first pieces of a stand‑alone LoRa® physical layer.  
Current focus is on reusable utility modules and a deterministic loopback
transmitter/receiver pair used for bring‑up and testing.

## Utility modules
- `gray` – encode/decode helpers and inverse table generation
- `whitening` – configurable LFSR with a PN9 helper
- `hamming` – CR 4/5 through 4/8 encode/decode tables
- `interleaver` – diagonal mapping and size helpers

## Loopback TX/RX
`lora/tx/loopback_tx.hpp` converts a payload into IQ samples, while
`lora/rx/loopback_rx.hpp` demodulates those samples back to bytes and checks the
CRC.  The end‑to‑end loopback exercise is demonstrated in
`tests/test_loopback.cpp`.

## Build
```bash
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## Dependencies
- `cmake` (>= 3.16), `ninja`, C++20 compiler
- `libliquid-dev` (Liquid-DSP FFT backend)
- `python3` (only for exporting reference vectors)

Optional (to generate GNU Radio reference vectors):
- `conda` or `mamba` with `gnuradio` 3.10 and `gnuradio-lora_sdr` available

## Environment Setup (conda)
If you use conda for the GNU Radio tooling, activate the dedicated environment before running any of the helper scripts:
```bash
# One-time creation (recommended)
conda create -n gnuradio-lora -c conda-forge gnuradio=3.10 cmake ninja -y

# Activate for current shell/session
# (ensure conda is initialized: source ~/miniconda3/etc/profile.d/conda.sh)
conda activate gnuradio-lora

# Optional: if you have multiple Pythons, guide scripts to use this env
export GR_PYTHON="$(conda run -n gnuradio-lora which python)"
```
Notes:
- Ensure the GNU Radio LoRa SDR module is available in the environment (e.g., `python -c "import gnuradio.lora_sdr"`).
- Scripts like `scripts/gr_run_vector.sh` will automatically activate `gnuradio-lora` if `~/miniconda3` exists.

On Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build libliquid-dev python3
```

## Run tests
```bash
cd build
ctest --output-on-failure             # run all tests
ctest -R Loopback --output-on-failure # run only loopback test
```

## CLI Decoder
Build the CLI and decode IQ files (float32 or CS16) with auto OS detection and header‑auto decode:
```bash
cmake --build build --target lora_decode

# Float32 IQ with preamble+sync+header+payload (OS=4 file)
./build/lora_decode --in vectors/sf7_cr45_iq_os4_hdr.bin --sf 7 --cr 45 --format f32

# Float32 IQ (generated locally)
# OS=1
./build/gen_frame_vectors --sf 7 --cr 45 --payload vectors/sf7_cr45_payload.bin --out /tmp/f_os1.bin --os 1 --preamble 8
./build/lora_decode --in /tmp/f_os1.bin --sf 7 --cr 45 --format f32

# OS=4 (includes two sync upchirps and two downchirps before the frame)
./build/gen_frame_vectors --sf 7 --cr 45 --payload vectors/sf7_cr45_payload.bin --out /tmp/f_os4.bin --os 4 --preamble 8
./build/lora_decode --in /tmp/f_os4.bin --sf 7 --cr 45 --format f32

# CS16 IQ
./build/lora_decode --in capture_cs16.bin --sf 7 --cr 45 --format cs16 --out payload.bin

# JSON output (machine‑readable): includes header, payload_hex, and status
./build/lora_decode --in vectors/sf7_cr45_iq_os4_hdr.bin --sf 7 --cr 45 --format f32 --json
```

Notes:
- Use `--print-header` to print header fields on stderr (human‑readable) in addition to the decoded payload.
- Use `--allow-partial` to return payload even if CRC fails (diagnostics). With `--json`, outputs a partial JSON with reason.
- Set `LORA_DEBUG=1` to print pre‑detected OS/phase/start on stderr.
```

## GNU Radio-like receive pipeline
For block-level experiments similar to the GNU Radio `lora_sdr` flowgraph, use
`lora::rx::pipeline::GnuRadioLikePipeline`. The pipeline mirrors the classic
chain (Frame Sync → FFT Demod → Gray Mapping → Deinterleaver → Hamming
Decoder → Dewhitening → Header/CRC) while relying on the Liquid-DSP FFT backend
shared by the rest of the project.

```cpp
#include <vector>
#include "lora/rx/gr_pipeline.hpp"

std::vector<std::complex<float>> iq = /* load or synthesize samples */;

lora::rx::pipeline::Config cfg;
cfg.sf = 7;
cfg.min_preamble_syms = 8;

lora::rx::pipeline::GnuRadioLikePipeline pipeline(cfg);
auto report = pipeline.run(iq);
if (!report.success) {
    std::fprintf(stderr, "RX failed: %s\n", report.failure_reason.c_str());
}

// Inspect intermediate stages (FFT bins, header CWs, payload bytes, etc.).
const auto& header_stage = report.header;
const auto& payload_stage = report.payload;
```

The `PipelineResult` contains per-stage buffers that mirror the GNU Radio block
boundaries, making it straightforward to compare against reference dumps or to
inject custom processing between stages.

## Reference vectors (optional)
Reference vectors are generated from the GNU Radio LoRa SDR blocks when available.
Recommended, isolated setup using conda:
```bash
conda create -n gnuradio-lora -c conda-forge gnuradio=3.10 cmake ninja -y
conda activate gnuradio-lora
```
Then generate vectors:
```bash
cmake --build build --target export_vectors
```
The vectors are stored under `vectors/` and copied into the build tree automatically.
If GNU Radio is not available, the export target will fail (by design) to ensure
vectors remain aligned with the true reference implementation.

Advanced options:
- Broaden SF/CR coverage or payload lengths in one go:
  - `PAIRS_EXTRA="9 45 10 48" LENGTHS="24 31 48" cmake --build build --target export_vectors`
- Generate local header-enabled OS4 IQ with selectable interpolation:
  - Polyphase (recommended):
    `./build/gen_frame_vectors --sf 7 --cr 45 --payload vectors/sf7_cr45_payload.bin --out /tmp/f_os4_poly.bin --os 4 --preamble 8 --interp poly`
  - Repeat (zero-order hold, synthetic):
    `./build/gen_frame_vectors --sf 7 --cr 45 --payload vectors/sf7_cr45_payload.bin --out /tmp/f_os4_rep.bin --os 4 --preamble 8 --interp repeat`

### Quick GR Taps & Comparison
- Generate GNU Radio taps for the canonical reference vector:
  ```bash
  bash scripts/gr_run_vector.sh
  # outputs under logs/: gr_hdr_gray.bin, gr_hdr_nibbles.bin, gr_predew.bin, gr_postdew.bin, gr_rx_payload.bin
  ```
- Compare LoRa Lite vs GNU Radio on the same vector:
  ```bash
  python3 scripts/run_vector_compare.py
  # writes JSON summary to logs/run_vector_compare.out and artifacts to logs/
  ```
- Convert LoRa Lite logs to header codewords and compare with GNU Radio:
  ```bash
  python3 scripts/lite_log_to_cw.py --log logs/lite_ld.json --gr-nibbles logs/gr_hdr_nibbles.bin
  # or run both steps automatically
  bash scripts/demo_lite_log_to_cw.sh
  ```

Note on GNU Radio Throttle:
- For batch vector generation we strip/remove `blocks_throttle` from the flowgraphs to avoid blocking, see `scripts/export_vectors_grc.sh` and `scripts/strip_throttle_blocks.py`.
- This does not affect signal content (preamble/header/payload) — only execution rate.

## Remaining MVP work
- Integrate a single FFT backend and precomputed chirps
- Complete TX/RX pipeline over preallocated buffers
- Cross-validate against `gr-lora_sdr` vectors and capture performance metrics
- Basic synchronization (preamble detection and timing/Fo offset) — initial implementation added (see Synchronization)

See [LoRa_Lite_Migration_Plan_README.md](LoRa_Lite_Migration_Plan_README.md)
for detailed roadmap and context.

## Synchronization (MVP)
- Preamble detection: detects a run of upchirps (OS=1) and returns the start sample.
- Sync-word check: validates the following symbol against the expected sync.
- CFO estimate + compensate: coarse estimate from preamble, phasor rotation before dechirp/FFT.
- STO (integer) estimate + align: searches small sample shifts to maximize correlation with upchirp, then realigns.

APIs (see `include/lora/rx/preamble.hpp`):
- `detect_preamble(ws, samples, sf, min_syms=8)` → `optional<size_t>` start sample
- `decode_with_preamble(ws, samples, sf, cr, payload_len, min_preamble_syms=8, expected_sync=0x34)`
- `decode_with_preamble_cfo(...)` and `decode_with_preamble_cfo_sto(...)` for CFO/STO handling

Run only the sync tests:
```bash
cd build
ctest -R Preamble --output-on-failure
```

## Oversampling (OS>1)
- Polyphase decimation (Liquid‑DSP `firdecim_crcf`) with Kaiser LPF converts oversampled IQ to OS=1 for detection/decoding.
- API: `detect_preamble_os(...)` returns start sample, OS, and phase; `decode_with_preamble_cfo_sto_os(...)` performs OS-aware decode.
- Optional OS4 vectors (GNU Radio) can be generated by `scripts/export_vectors.sh` when GNU Radio is available; stored as `sf*_cr*_iq_os4.bin`.

## Next Steps
- Validate OS>1 on real GNU Radio/SDR captures (not only synthetic repeat).
- Add explicit header parsing in RX (length/CR/header CRC) and dewhiten entire frame; derive payload length from header.
- Add tests for header parsing and end-to-end with header-enabled vectors.

## Cross-Validation & Benchmarks
- Run cross-validation against GNU Radio TX-PDU (OS4) across many SF/CR/lengths with random payloads; produces CSV and optional heatmaps:
```bash
# Quick run
CROSS_REPS=1 python3 scripts/cross_validate_benchmark.py

# Control ranges (comma-separated) and repetitions
CROSS_SFS="7,8,9" CROSS_CRS="45,48" CROSS_LENGTHS="16,48" CROSS_REPS=2 \
  python3 scripts/cross_validate_benchmark.py

# Outputs under build/reports/:
#  - cross_validate.csv
#  - success_rate_heatmap.png, decode_time_heatmap.png (if matplotlib available)
```

## Current Status: Header Alignment (2025-09-10)
- RX header path now supports GR-style mapping for the standard LoRa header (CR=4/8):
  - corr = (raw − 44) mod N → gray(symbol)
  - GR mapping: gnu = ((gray + (1<<sf) − 1) & (N−1)) >> 2, with sf_app = sf − 2
  - Deinterleaver geometry matches GR (diagonal), Hamming(8,4) over exactly 5×2 codewords
  - Added intra-symbol bit_shift search to find the correct nibble boundary
- Current issue: header checksum still fails on the clean OS=4 vector; GR-direct bytes observed: `8b b9 69 f4 0c` (and swapped `b8 9b 96 4f c0`).

### Next steps
- Align TX header emission to match the GR header bit layout exactly:
  - Emit sf_app=sf−2 bits per header symbol (post `(gray−1)/4`) into the interleaver columns
  - Use the same diagonal interleaver as RX/GR for header (CR=4/8) before modulating
- After TX alignment, re-run the reference script:
```bash
bash scripts/temp/run_ref_os4_sfdq.sh
```
- Expectation: standard header parses; then payload CRC should validate on the clean generator vector.
