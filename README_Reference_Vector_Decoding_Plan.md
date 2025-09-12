# Reference Vector Decoding — Action Plan

This plan consolidates findings from `PROJECT_NOTES.md`, `README_LoRa_Lite_GR_Compatibility.md`, and “Practical Solutions for the "Reference Vector Decoding" Issue.md”. It provides a clear, actionable roadmap to resolve the remaining gaps in decoding the reference vector(s), with emphasis on the explicit LoRa header (5 bytes, CR=4/8) and end‑to‑end payload CRC.

## Current Snapshot
- Goal: Full behavioral match between LoRa Lite and GNU Radio’s `gr-lora_sdr`.
- Proven parity: preamble detection, sync, FFT scaling, Gray mapping, payload FEC/interleaver, and whitening path — matching GR when the header is correct.
- Remaining gap (vector OS≈2 at SR=250k/BW=125k): header block 1 (symbols 8–15) mis-assembled causes header checksum failure.
  - Ground truth header CWs (SF7) from GR (`logs/gr_deint_bits.bin`):
    - Block0: `00 74 C5 00 C5`
    - Block1: `1D 12 1B 12 00`
- Practical consideration (second track): on some vectors where the header already parses, payload CRC mismatches can arise from byte assembly, whitening boundaries, or CRC endianness.

## What GR Does (confirmed header path)
Explicit header, CR=4/8, SF=7 → `sf_app = 5`:
1) Symbol reduction: `gnu = ((raw_bin - 1) mod 2^SF) >> 2` (drop 2 LSBs after −1). Do not carry −44 at the header reduction stage.
2) Gray encode `gnu` (take `sf_app` bits).
3) Build `inter[i][j]` MSB→LSB for `sf_app` bits per symbol.
4) Diagonal deinterleaver per 8‑symbol block (state resets per block): `deinter[(i - j - 1) mod sf_app][i] = inter[i][j]`.
5) Emit rows MSB→LSB → 5 CW bytes per block → 10 total → Hamming(8,4) → 10 nibbles → 5‑byte header → checksum.

Important: Treat block 1 as an independent block (fresh origin). Do not carry block 0 state.

## Two‑Track Fix Strategy

### Track A — Header Block‑1 Alignment (reference vector OS≈2)
- Deterministic timing anchor:
  - Start header at `sync + 2 downchirps + 0.25 symbol` (2.25 symbols after sync).
  - Apply measured CFO/STO (from preamble) before sampling header symbols.
- Minimal bounded retry on checksum failure:
  - Sample nudges of ±1/64, ±1/32, ±1/16 symbol, and symbol offsets ±1..±2. Stop at first valid checksum.
- Exact GR mapping per block (reset per block):
  1) `gnu = ((raw - 1) & (N-1)) >> 2`
  2) Gray(gnu), take `sf_app` MSB→LSB bits per symbol
  3) `deinter[(i - j - 1) mod sf_app][i] = inter[i][j]`
  4) Rows→bytes (5 CW/blk) ×2 → Hamming(8,4) → 10 nibbles → 5‑byte header → checksum
- Validation hooks:
  - Log 10 CW bytes and assert exact match: `00 74 C5 00 C5 1D 12 1B 12 00`.
  - Header fields: `{len=11, cr=1 (4/5), has_crc=1}`.
- Tools to converge offline before “baking” into C++:
  - Generate GR taps: `bash scripts/gr_run_vector.sh`
  - Scan header timing: `./build/lora_decode ... --hdr-scan [--hdr-scan-narrow] [--hdr-scan-fine]`
  - Analyze best windows + variants: `python3 scripts/from_lite_dbg_hdr_to_cw.py`
  - Parse best from logs: `python3 scripts/scan_hdr_gr_cw.py`
- Bake the deterministic anchor/mapping into `src/rx/frame.cpp` once a 10/10 CW match appears; remove wide scans from runtime path.

### Track B — End‑to‑End Payload CRC (vectors where header already parses)
Use the “Practical Solutions” checks to pin down post‑header issues:
- Bit/nibble assembly:
  - MSB‑first within each symbol/nibble throughout.
  - Be explicit about header vs payload byte assembly. Implement helpers for assembling bytes from two nibbles in the required order and use consistently. Validate against GR dumps.
- Whitening boundaries and parameters:
  - Do not whiten the explicit header (5 bytes).
  - Start whitening right at the payload start; exclude the two CRC bytes from whitening.
  - Keep the project’s proven PN9 whitening parameters (as already aligned with GR in this repo): polynomial `x^9 + x^5 + 1`, seed `0x1FF`, MSB‑first mask. If you experiment with alternative 8‑bit descriptions, cross‑check results identically match the PN9 path already validated here.
  - Practical check: dump pre/post‑dewhitening payload bytes; ensure the last two (CRC field) remain unchanged by dewhitening.
- CRC algorithm and endianness:
  - CRC‑CCITT‑FALSE over payload bytes only (exclude the 2 CRC bytes), init `0xFFFF`, poly `0x1021`.
  - Trailer endianness on the wire is commonly little‑endian; verify against GR payload dumps by logging both BE/LE interpretations and locking the matching one.

## Minimal Test Matrix
- Canonical vector: `vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown` (SF7/CR45/Sync 0x12):
  - Expect exact header CW match and valid header checksum.
  - Decode payload, verify CRC OK.
- Sanity vectors: lengths {0,1,2,11,16}, CR∈{4/5,4/6,4/7,4/8}, `impl_header=true/false`, Sync {0x12, 0x34}:
  - Spot‑check payload nibble packing, whitening boundaries, and CRC endianness.

## How to Run
- GNU Radio taps (ground truth):
  - `bash scripts/gr_run_vector.sh`
  - Inspect: `logs/gr_hdr_gray.bin`, `logs/gr_deint_bits.bin`, `logs/gr_hdr_nibbles.bin`, `logs/gr_rx_payload.bin`
- LoRa Lite header scan and analysis:
  - `./build/lora_decode --in vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown --sf 7 --cr 45 --sync 0x12 --min-preamble 8 --hdr-scan`
  - `python3 scripts/from_lite_dbg_hdr_to_cw.py`
  - `python3 scripts/scan_hdr_gr_cw.py`
- One‑shot comparison harness:
  - `python3 scripts/run_vector_compare.py`

## Success Criteria
- Header CW bytes equal GR exactly: `00 74 C5 00 C5 1D 12 1B 12 00`.
- `parse_standard_lora_header` passes with `{len=11, cr=1, has_crc=1}`.
- Payload CRC verifies; payload bytes match GR taps (pre/post dewhitening where applicable).

## Notes and Tips
- Always apply CFO/STO corrections before sampling header symbols.
- Never carry deinterleaver origin from block 0 into block 1; reset per block.
- For payload CRC investigations, log byte streams at each stage: post‑Hamming, post‑dewhitening, and CRC check inputs; compare to GR.
- Keep the bounded header retry small to maintain performance (few offsets only); stop at first valid checksum.

---
For deeper tracking details (timing grids, variant parameters, and specific runs), see `README_LoRa_Lite_GR_Compatibility.md` and `PROJECT_NOTES.md`. The “Practical Solutions” checklist above is incorporated under Track B to harden the payload path once header alignment is locked.

---
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

## Refactor status and runtime flags

- Modules split for maintainability:
  - `include/lora/rx/header_decode.hpp`, `src/rx/header_decode.cpp`: header decode path (OS-aware align, GR per-block mapping, bounded variants). Heavy scan is here and off by default.
  - `include/lora/rx/payload_decode.hpp`, `src/rx/payload_decode.cpp`: payload decode (FEC, dewhitening, CRC where applicable).
  - `include/lora/rx/demod.hpp`: shared `demod_symbol_peak(...)` helper.
  - `src/rx/frame.cpp`: orchestrator; heavy header scans removed from default path.

- Runtime flags:
  - `LORA_HDR_IMPL=1`: enable modular header implementation (header_decode.cpp).
  - `LORA_HDR_SCAN=1`: enable heavy header scan/variants for debugging.
  - `LORA_DEBUG=1`: verbose logs (preamble/symbol traces).

- Suggested runs:
  - Deterministic: `./build/lora_decode ...`
  - Modular header: `LORA_HDR_IMPL=1 ./build/lora_decode ...`
  - With scans: `LORA_HDR_IMPL=1 LORA_HDR_SCAN=1 ./build/lora_decode ...`

## Execution Log

See [docs/reference_vector_exec_log.md](docs/reference_vector_exec_log.md) for run history.
