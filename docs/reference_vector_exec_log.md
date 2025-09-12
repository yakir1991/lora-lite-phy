# Reference Vector Decoding — Execution Log

## Execution Log — 2025-09-12

- Environment: Activated conda `gnuradio-lora` for GNU Radio scripts. Verified `gnuradio.lora_sdr` import OK. LoRa Lite built target `lora_decode`.

- GR taps (canonical vector):
  - Ran: `bash scripts/gr_run_vector.sh`
  - Result: logs refreshed: `logs/gr_hdr_gray.bin`, `logs/gr_deint_bits.bin`, `logs/gr_hdr_nibbles.bin`, `logs/gr_predew.bin`, `logs/gr_postdew.bin`, `logs/gr_rx_payload.bin`.

- LoRa Lite header scan (narrow around deterministic anchor):
  - Ran: `./build/lora_decode --in vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown --sf 7 --cr 45 --sync 0x12 --min-preamble 8 --hdr-scan --hdr-scan-narrow --hdr-off0 2 --hdr-off1 0 --hdr-off0-span 1 --hdr-off1-span 1`
  - Output: `logs/lite_hdr_scan.json` (3×11×3×11 combinations); stderr captured to `logs/hdr_scan_targeted.err`.
  - Analysis: `python3 scripts/from_lite_dbg_hdr_to_cw.py`
    - Best timing-only CW vs GR (`logs/gr_deint_bits.bin`):
      - Params: `off0=2 samp0=0 off1=-1 samp1=0`
      - Lite CW: `00 74 c5 00 c5 03 92 a5 1d b2`
      - GR   CW: `00 74 c5 00 c5 1d 12 1b 12 00`
      - full_diff = 5 (block0 exact; block1 mismatched)
    - Block1 variant sweep (diag/row/col variants across top timing windows):
      - Best: `diagshift=0 rot1=3 rowrev=0 colrev=0 colshift=0`
      - CW: `00 74 c5 00 c5 1d b2 03 92 a5`
      - full_diff = 4, last5_diff = 4

- LoRa Lite header scan — fine around anchor (dense sample shifts):
  - Ran: `./build/lora_decode ... --hdr-scan --hdr-scan-narrow --hdr-scan-fine --hdr-off0 2 --hdr-off1 0 --hdr-off0-span 2 --hdr-off1-span 2`
  - Output: `logs/lite_hdr_scan.json` (~5×65×4×65 combinations)
  - Analysis: `python3 scripts/from_lite_dbg_hdr_to_cw.py`
    - Best timing-only CW:
      - Params: `off0=2 samp0=0 off1=-1 samp1=-32`
      - Lite CW: `00 74 c5 00 c5 50 aa 29 db 93`
      - GR   CW: `00 74 c5 00 c5 1d 12 1b 12 00`
      - full_diff = 5 (block0 exact; block1 still mismatched)
    - Best block1-variants across top timings:
      - Params: `off0=2 samp0=0 off1=2 samp1=-28`, variant `rot1=2 rowrev1=1 colrev1=0`
      - Lite CW: `00 74 c5 00 c5 1d d1 1b 56 a5`
      - full_diff = 3, last5_diff = 3

  Interim: Deterministic timing is right; mismatch localized to block1 assembly. Next change is to “bake” GR’s per-block mapping with a fresh deinterleave origin for block1 and remove variant/state leakage.

- One-shot comparison harness:
  - Ran: `python3 scripts/run_vector_compare.py`
  - Result summary:
    - LoRa Lite `lora_decode`: exit code 7 (fec_decode_failed), JSON not emitted (header path still diverges).
    - GNU Radio RX-only: ok, `rx_payload_len` = 230 bytes; taps written under `logs/`.

- Interim conclusion:
  - Deterministic timing anchor at `sync + 2.25` symbols is working; block0 CWs match GR exactly under `gnu=((raw-1)&(N-1))>>2` and GR diagonal deinterleaver.
  - Block1 is still mis-assembled (4/5 byte mismatches). Next step per Track A is to bake block‑reset deinterleave/mapping for block1 (fresh origin) and remove any state carryover; validate with exact 10/10 CW match before removing wide scans.

- Code attempt (bounded two-block header search):
  - Implemented a small, deterministic retry inside `decode_header_with_preamble_cfo_sto_os` that:
    - Anchors header at `sync + 2*N + N/4` and tries `off0∈{1,2,3}`, `off1∈{-1,0,1}` with sample nudges `{0, ±N/64, ±N/32}` per block independently.
    - Uses GR mapping per block: `gnu=((raw−1) mod N)>>2` → `Gray(gnu)` → diagonal deinterleave `r=(i−j−1) mod (sf−2)`; rows→bytes; Hamming(8,4) → 10 nibbles → 5 bytes → `parse_standard_lora_header`.
  - Outcome: no valid header yet on the canonical vector (consistent with scan analysis showing 3–5 CW byte mismatch on block1).

## Execution Log — 2025-09-12

- Environment: Built CLI (`build/lora_decode`) successfully after fixing `hdr_opt` scope in `src/rx/frame.cpp` (initialized before header attempts). Generator mismatch resolved by reconfiguring `cmake` with existing generator.

- GR taps (canonical vector):
  - Ran: `bash scripts/gr_run_vector.sh`
  - Result: refreshed `logs/gr_hdr_gray.bin`, `logs/gr_deint_bits.bin`, `logs/gr_hdr_nibbles.bin`, `logs/gr_predew.bin`, `logs/gr_postdew.bin`, `logs/gr_rx_payload.bin`.

- LoRa Lite header scan (narrow around deterministic anchor):
  - Ran: `./build/lora_decode --in vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown --sf 7 --cr 45 --sync 0x12 --min-preamble 8 --hdr-scan --hdr-scan-narrow --hdr-off0 2 --hdr-off1 0 --hdr-off0-span 1 --hdr-off1-span 1 2> logs/hdr_scan_targeted.err`
  - Output: `logs/lite_hdr_scan.json`.
  - Analysis: `python3 scripts/from_lite_dbg_hdr_to_cw.py`
    - Best with block1 variants across top timings:
      - Params: `off0=2 samp0=0 off1=-1 samp1=0`
      - Variant: `diagshift=0 rot1=3 rowrev=0 colrev=0 colshift=0`
      - CW: `00 74 c5 00 c5 1d b2 03 92 a5`
      - GR CW: `00 74 C5 00 C5 1D 12 1B 12 00`
      - full_diff=4, last5_diff=4 (block0 exact; block1 mismatched)

- One‑shot comparison harness:
  - Tried: `python3 scripts/run_vector_compare.py` (captured into logs)
  - Lite JSON shows header parsed but payload decode failed: `{ "success": false, "step": 111, "reason": "fec_decode_failed", "sf": 7, "cr": 45, "detect_os": 2, "header": {"len": 66, "cr": 2, "crc": true} }` (from `logs/lite_ld.json`).

- Interim conclusion:
  - Deterministic timing anchor and GR mapping produce exact Block0 CWs; Block1 still mis‑assembled (4/5 mismatches) consistent with prior notes. Next action per Track A: bake per‑block reset GR mapping for Block1 (fresh deinterleave origin, no state leakage) into the runtime path and re‑validate for 10/10 CW match before removing scans.

- Targeted two‑block hdr‑single attempt (best known params):
  - Ran: `./build/lora_decode --in vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown --sf 7 --cr 45 --sync 0x12 --min-preamble 8 --hdr-single --hdr-off0-single 2 --hdr-samp0 0 --hdr-off1-single -1 --hdr-samp1 0`
  - Observed CWs:
    - raw: `00 74 c5 00 c5 03 92 a5 1d b2` → Hamming decode failed early (k=1)
    - corr: `13 a1 e5 98 05 60 26 a3 0f bc` → Hamming decode failed (k=4)
  - Interpretation: block0 exact, block1 cw bytes invalid under CR=4/8 (mapping/assembly mismatch), consistent with scan analysis.

- Next actions (Track A):
  - Implement block‑1 specific GR mapping with explicit fresh origin in runtime header path and include a minimal diagshift/row rotation attempt bounded to a few candidates; stop on first valid checksum. Re‑run canonical vector expecting exact 10/10 CW and valid header checksum.

- Baseline JSON decode (canonical vector):
  - Ran: `./build/lora_decode --in vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown --sf 7 --cr 45 --sync 0x12 --min-preamble 8 --json > logs/lite_ld.json 2> logs/lite_ld.err`
  - Latest record in `logs/lite_ld.json`:
    `{ "success": false, "step": 111, "reason": "fec_decode_failed", "sf": 7, "cr": 45, "detect_os": 2, "header": {"len": 66, "cr": 2, "crc": true} }`
  - Interpretation: header parsed (len=66, CR payload=CR46), but payload FEC failed; consistent with Block1 CW mismatch in header path causing downstream misalignment on some runs.

- Scan summary (from `logs/from_lite_dbg_hdr_to_cw.out`):
  - Timing-only best: `off0=2 samp0=0 off1=-1 samp1=0`, lite CW `00 74 c5 00 c5 03 92 a5 1d b2` (full_diff=5)
  - With block1 variants: `diagshift=0 rot1=3 rowrev=0 colrev=0 colshift=0`, CW `00 74 c5 00 c5 1d b2 03 92 a5` (full_diff=4, last5_diff=4)

## Execution Log — 2025-09-12 (Refactor updates)

- Code refactor for maintainability and focused header work:
  - Moved header decode logic into `src/rx/header_decode.cpp` with public API in `include/lora/rx/header_decode.hpp`.
  - Moved payload decode into `src/rx/payload_decode.cpp` with API `include/lora/rx/payload_decode.hpp`.
  - Introduced shared `include/lora/rx/demod.hpp` helper for `demod_symbol_peak(...)`.
  - Kept `src/rx/frame.cpp` as orchestrator; removed heavy header scans from the default runtime path.

- Flags and behavior:
  - `LORA_HDR_IMPL=1`: enable modular header decode path in `header_decode.cpp`.
  - `LORA_HDR_SCAN=1`: enable heavy header scan/variants (debug only). Default off.
  - `LORA_DEBUG=1`: enable verbose logs (preamble/symbol traces, etc.).

- Build status:
  - Reconfigured and built successfully (`cmake -S . -B build -G Ninja` + `cmake --build build`).
  - Default path is quiet and deterministic; scans centralized behind flags in `header_decode.cpp`.

---

## Execution Log — 2025-09-12
- Applied MSB-first symbol bit extraction for payload in `src/rx/demodulator.cpp`.
- Changed dewhitening to exclude CRC trailer (payload-only PN9) in `src/rx/demodulator.cpp`.
- Switched CRC check to compute CRC-CCITT-FALSE over dewhitened payload and accept LE/BE trailer ordering; still expect LE to match.
- Build and run (canonical vector) result: header parsed `{len=66, cr=2, has_crc=true}` but FEC decode failed for payload (`exit step=111`). This indicates header block-1 mapping still diverges earlier on some runs; proceeding to bake GR-accurate per-block mapping with block-1 reset in `src/rx/frame.cpp` per Track A of the plan.
- Next: implement GR mapping reset for header block-1 in `src/rx/frame.cpp`, re-run header CW comparison, then re-verify payload CRC path on vectors where header parses.
- Applied GR-accurate header mapping per block in `src/rx/frame.cpp` (gnu=((raw-1)&(N-1))>>2 → Gray(gnu) → take sf_app MSB→LSB; deinterleave r=(i-j-1) mod sf_app; header bytes assembled as high-nibble first). Preparing build+run of the canonical vector to validate 10/10 CW match and stable header parse without wide scans.

