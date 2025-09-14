### 2025-09-14 — Per-symbol FD-per and sFCFO experiments: conclusions

- Key observations:
  - Block-1 fractional-bin δ and phase-slope ν are large and inconsistent (|δ| up to ~0.48, ν up to ~0.49), flipping signs across symbols; Block-0 remains small/stable.
  - Block CFO estimates are small and steady (eps1 ≈ 0.0068–0.0195 bins), so CFO is not the root cause.
  - Per-symbol fractional CFO and FD-per (k ∈ {±0.35, ±0.75}) barely change the demodulated g1 sequences, and header CRC still fails. This indicates our compensation is not acting at the right place (needs true time-domain fractional delay/resampling).
  - Gray/whitening/CRC toggles do not affect diff_block1, confirming the issue is pre-mapping demod/timing.
- Conclusion:
  - The primary problem is intra-symbol micro-timing/SFO (fractional delay) within Block-1, not CFO or mapping. We likely need a per-symbol fractional-delay filter (time-domain resampler) before dechirp, rather than phase-only tweaks.
- Next immediate checks:
  - Parity test for OS=2: run the same anchor with LORA_HDR_BASE_SAMP_OFF = 71 and 73 and compare δ trends vs 72. If parity toggling shifts δ/ν patterns materially, we have an OS-parity/rounding sensitivity to fix.

• Parity check (OS=2, sa=71 vs sa=73):
  - Files: `logs/parity_sa71.json/.term`, `logs/parity_sa73.json/.term`
  - Result: Both report `success=false` (`header_crc_failed`). `sa71` header=null; `sa73` parsed a header object but CRC=false. `[hdr-block-cfo]` eps1 small in both; `[hdr-frac]` for Block‑1 remains large/inconsistent.
  - Takeaway: ±1 sample parity did not resolve Block‑1. The issue is not base-index parity but intra‑symbol fractional delay/SFO that needs time‑domain resampling before dechirp.
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
    - GR taps cross-check:
      - `gr_deint_bits.bin`: `00 74 c5 00 c5 1d 12 1b 12 00`
      - `gr_hdr_nibbles.bin`: `00 0e 03 00 03 07 09 0b 09 00`
      - `gr_hdr_gray.bin` (first 32): header symbol Gray stream dumped by GR
    - Continuous-origin deinterleave (no per-block reset) test:
      - CW: `00 74 c5 00 c5 a5 1d b2 03 92` (does not match GR)
      - Conclusion: GR resets origin per 8-symbol block; divergence is within block‑1 row/column arrangement.

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

- Exhaustive block‑1 mapping probe (offline on best timing):
  - Search space: alt diagonal orientation, diagshift∈[0..4], all 5! row permutations, column reversal, and circular column shifts 0..7.
  - Result: no exact 10/10 match found. Best candidate: `alt_diag=false diagshift=0 perm=(3,0,1,2,4) colrev=0 colshift=0` with CW `00 74 c5 00 c5 1d 03 92 a5 b2` (diff=4).
  - Implication: raw‑to‑gnu reduction is correct, and block0 mapping is correct; block1 likely requires a specific intra‑byte/nibble arrangement or a subtle per‑symbol bit order nuance not captured by these variants. Next step: compare Lite’s per‑symbol bit extraction vs GR’s gray-to-bits path symbol‑by‑symbol using `logs/gr_hdr_gray.bin` for precise alignment.

- Bit-order/diagonal orientation probe (block‑1 only):
  - Tried MSB-first vs LSB-first per symbol and alt-diagonal (r=(i+j) mod sf_app) with diagshift∈[0..4].
  - Best result: MSB-first, standard diagonal, diagshift=2 → CW `00 74 c5 00 c5 1d b2 03 92 a5` (diff=4), same as earlier variant family.
  - Conclusion: The remaining 4 mismatches persist; focus next on byte/nibble assembly order and exact bit extraction path against GR dumps (rows from `gr_hdr_gray.bin`).

- Block‑1 inter-bit circular shift probe (MSB-first Gray(gnu) bits before deinterleave):
  - Tried circular shifts k∈{1..sf_app−1} over the column-major stream [i then j].
  - Best shift: k=1 → CW `00 74 c5 00 c5 36 01 93 b5 15` (diff=5). No exact match.
  - This suggests the issue is not a simple intra-symbol bit shift before deinterleave.

- GNU-match scan over hdr-scan (scripts/scan_hdr_gnu_match.py):
  - GR-derived block1 gnu: `GNU1 [0, 0, 0, 22, 12, 1, 5, 14]`.
  - exact_block1: False; exact_both: False.
  - best_b1_diff: 6 at params `{'off0':0,'samp0':-32,'off1':-1,'samp1':1}` with `best_b1_gnu [27, 0, 6, 13, 31, 8, 17, 14]`.
  - best_both_totdiff: 6 at params `{'off0':2,'samp0':0,'off1':-1,'samp1':1}`.
  - Interpretation: mismatch exists already at the gnu=(raw−1)>>2 stage for block1 across all scanned offsets; mapping permutations alone cannot fix it. Next: compare per-symbol Gray/gnu vs `logs/gr_hdr_gray.bin` and analyze raw-bin deltas to identify timing/CFO-related shifts specific to block1.

- Symbol-by-symbol comparison vs GR (scripts/compare_gray_bits_vs_gr.py):
  - Compared GR Gray/bin/bits/gnu per symbol to Lite `raw`/`lite_gnu` at `off0=2,samp0=0,off1=-1,samp1=0`.
  - Findings (delta_raw = (lite_raw − (4*GR_gnu+1)) mod N):
    - Block0 deltas (i=0..7): 76, 40, 96, 48, 4, 84, 4, 20.
    - Block1 deltas (i=8..15): 56, 8, 124, 113, 72, 88, 10, 67.
  - Insight: deltas are not constant across symbols in either block; mapping-only fixes cannot resolve this. This points to residual per-symbol timing/phase misalignment at header capture, especially impacting block1. Next step: targeted per-symbol micro-shifts for block1 (±N/128, ±N/64 neighborhood) with early‑exit on header checksum match, bounded to preserve performance.

- Block‑1 micro‑shift search (runtime path; LORA_HDR_MICRO=1):
  - Ran: `LORA_HDR_MICRO=1 ./build/lora_decode --in vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown --sf 7 --cr 45 --sync 0x12 --min-preamble 8 --print-header`
  - Result: `[header] len=182 cr=1 crc=true` then `Decode failed (step=109, reason=header_crc_failed)`.
  - Interpretation: no valid checksum found within the bounded micro‑shift/coarse offsets grid yet; the parsed header bytes indicate wrong nibble assembly under the tried windows.
  - Next: run a targeted hdr‑single grid over `off1∈{-2..2}` and `samp1∈{0,±N/64,±N/32}` (keeping `off0=2,samp0=0`) to print CW bytes (raw/corr) for inspection, then refine the deterministic anchor for block1.

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


### 2025-09-13 - Block1 mapping variants, micro progress logging, forced header

- Ran quick JSON decode (post-changes):
  - cmd:
    - `./build/lora_decode --in vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown --sf 7 --cr 45 --sync 0x12 --min-preamble 8 --json > logs/quick.json 2> logs/quick.err`
  - result:
    - quick.json: `reason: header_crc_failed (step=109)`, header parsed: `{len=182, cr=1, crc=true}`
    - quick.err: `[header] len=182 cr=1 crc=true`

- Ran hdr-single at two deterministic anchors:
  - off0=2,samp0=0; off1=-1,samp1=0 → err shows CWs:
    - raw: `00 74 c5 00 c5 03 92 a5 1d b2` (Hamming fail at 1)
    - corr: `13 a1 e5 98 05 60 26 a3 0f bc` (Hamming fail at 4)
  - off0=2,samp0=0; off1=0,samp1=0 → err shows CWs:
    - raw: `00 74 c5 00 c5 25 4b 3b 65 06` (Hamming fail at 1)
    - corr: `13 a1 e5 98 05 4c 46 1e 79 c0` (Hamming fail at 4)

- Targeted hdr-scan + offline analysis:
  - hdr-scan narrow around off0=2, off1=0 with sample shifts → generated `logs/lite_hdr_scan.json`
  - `scripts/from_lite_dbg_hdr_to_cw.py` results:
    - Best timing-only: `full_diff=5`, best params: `{off0:2, samp0:0, off1:-1, samp1:0}`
      - lite cw: `00 74 c5 00 c5 03 92 a5 1d b2`
      - gr   cw: `00 74 c5 00 c5 1d 12 1b 12 00`
    - Best with block1 variants (diagshift/rot/row/col/colshift search over top-1089 timings):
      - params: `{off0:2, samp0:0, off1:-1, samp1:0}`, `diagshift=0 rot1=3 rowrev=0 colrev=0 colshift=0`
      - `full_diff=4 last5_diff=4`
      - cwbytes: `00 74 c5 00 c5 1d b2 03 92 a5`

- Micro-run with progress logging (60s):
  - cmd:
    - `timeout 60s env LORA_HDR_MICRO=1 LORA_HDR_MICRO_WIDE=1 LORA_HDR_SLOPE=1 LORA_HDR_PROGRESS=1 LORA_HDR_PROGRESS_STEP=20000 ./build/lora_decode --in vectors/... --sf 7 --cr 45 --sync 0x12 --min-preamble 8 --print-header > logs/lite_run_micro.out 2> logs/lite_run_micro.err || true`
  - result:
    - lite_run_micro.out: `DEBUG: [impl] decode_header_with_preamble_cfo_sto_os_impl`
    - lite_run_micro.err: frequent `[hdr-micro] progress ...` updates up to ~4.4M iterations; no header OK found; final failure consistent with `header_crc_failed`.

- Forced Header (Track-2 payload pipeline sanity):
  - cmd:
    - `env LORA_DEBUG=1 ./build/lora_decode --in vectors/... --sf 7 --cr 45 --sync 0x12 --min-preamble 8 --force-hdr-len 182 --force-hdr-cr 1 --force-hdr-crc 1 --json > logs/force.json 2> logs/force.err`
  - result:
    - force.json: `{ success:false, reason:"header_forced", detect_os:2, detect_start:33250, header:{len:182,cr:1,crc:true}, payload_len:0 }`
    - force.err: preamble debug lines routed to stderr as expected.

- Code changes done in this session:
  - `src/rx/header_decode.cpp`:
    - Added per-symbol micro progress logging (guarded by `LORA_HDR_PROGRESS[_STEP]`).
    - Added Block1 mapping variants: `diagshift` (±1..2), column shifts over diagshifted block, and CFO-only mapping on Block1 in addition to both-blocks CFO mapping.
  - `src/rx/preamble.cpp`:
    - Redirected preamble debug prints to stderr when `LORA_DEBUG` is set to avoid corrupting JSON.

- Conclusions (so far):
  - Block0 CWs are stable and correct; Block1 remains mismatched by ~5 bits across a broad timing/micro search. New mapping variants did not yield a header CRC pass on the canonical SF7 vector.
  - Forced header confirms preamble/OS detect stable and JSON now clean. Next step: tighten Block1 timing alignment using GNU-derived gnu vectors per symbol to select per-symbol micro-shifts (script-driven ds), and/or align using deterministic sync+downchirp anchor inside the C++ path with symbol-wise fine search.

- Next actions:
  1) Implement symbol-wise gnu-guided micro alignment for Block1 (use script-derived `gnu` as target to pick ds maximizing nibble agreement before Hamming), then re-test.
  2) If still failing, instrument per-symbol raw→gnu deltas and dump to logs for direct comparison with `logs/gr_hdr_gray.bin` via `scripts/compare_gray_bits_vs_gr.py`.

### 2025-09-13 (later) - Auto timing scan and anchor probe, findings, and plan

- Auto timing script:
  - Added `scripts/auto_find_timing.py` to run `--hdr-scan --hdr-scan-fine` and parse best timing/variant. Produces `logs/auto_timing.json` and prints a suggested `--hdr-single` command.
  - Result on canonical vector:
    - best_timing: `{off0:2, samp0:0, off1:1, samp1:-56}`
    - best_variant: `{off0:2, samp0:0, off1:5, samp1:-61}` (variant diagshift/colrev/colshift improved to diff≈3 earlier)

- Auto end-to-end alignment script:
  - Added `scripts/auto_align_header.py` that orchestrates: hdr-scan → parse best → hdr-single (best, variant) → anchor grid probe (SYM,SAMP) → full decode. Logs under `logs/` and report to `logs/auto_align_report.json`.
  - Run summary:
    - hdr-single at best_timing and best_variant: both still fail Hamming on block1 (k=1/k=4). cwbytes examples: `00 74 c5 00 c5 78 20 b3 12 94` (raw).
    - Anchor grid probe around best_timing with `SYM∈[-2..2]`, `SAMP∈{-96..96}` (steps 16): parsed top results remain diff≈4 at `off1=1,samp1=-56` repeatedly.
    - Full decode (`--json`): still `header_crc_failed` (step=109). Header fields parsed remain `{len=182, cr=1, crc=true}`.

- Conclusions (updated):
  - No mapping issue: Block0 is consistently correct; Block1 improves only with timing shifts (off1≈1..5, samp1≈-56..-61) but does not reach CRC pass (diff≈3–4 persists). This strongly indicates a residual timing/phase drift between block0 and block1 that our current anchor at `sync + 2.25` and bounded micro-shifts do not fully correct.
  - Forced-gnu attempt (earlier) did not validate due to feeding 5-bit gnu into a builder that expects full-bin; not a reliable disproof of timing.

- Plan (next steps):
  1) Runtime anchor refinement: integrate automatic header-base sweep in C++ (around `sync+2.25N`) over `SYM_OFF∈[-2..2]` and `SAMP_OFF` in a small window; pick the anchor that maximizes block1 Hamming-valid CW count before header checksum.
  2) Fine-CFO per block: extend fractional CFO compensation for block1 (±{1/16,1/8,1/4} bins) and optionally a small linear slope across the 8 symbols; select the set that maximizes Hamming-valid CWs.
  3) gnu-guided per-symbol selection: when `LORA_HDR_GNU_B1`/known target exists, choose per-symbol micro-shifts to match gnu targets; emit `[hdr-guided] target/chosen` and selected cwbytes into stderr for verification.
  4) Diagnostics: log gnu per 16 header symbols and the chosen anchor/Fine-CFO to `logs/` for post-run diff against GR dumps; keep JSON output clean.
  5) Iterate anchor+Fine-CFO search bounded (deterministic small sets) and stop on first header checksum pass; only then proceed to payload.

- Tools added this session:
  - `scripts/auto_find_timing.py` (timing-only scan and parse).
  - `scripts/auto_align_header.py` (end-to-end auto alignment). Both print progress to terminal and write JSON/logs.

### 2025-09-13 (later) — LORA_HDR_LOG_GNU instrumentation results

- Run: `env LORA_HDR_LOG_GNU=1 ./build/lora_decode --in vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown --sf 7 --cr 45 --sync 0x12 --min-preamble 8 --json > logs/post_update.json 2> logs/post_update.err`
- Observations (stderr excerpts):
  - Deterministic anchors tried internally:
    - off0=2,samp0=0; off1=-1,samp1=0 → `b0: 13 31 15 0 27 0 6 13 | b1: 0 6 13 31 8 17 13 18`
    - off0=2,samp0=0; off1=0,samp1=0  → `b0: 13 31 15 0 27 0 6 13 | b1: 6 13 31 8 17 13 18 15`
    - off0=2,samp0=0; off1=5,samp1=-61 → `b0: 13 31 15 0 27 0 6 13 | b1: 30 30 0 0 14 5 14 6`
  - Fine‑CFO (±1/8, ±1/4 bin) on block‑1 nudges at most 1–2 entries by ±1 and does not converge to GR’s target.
  - CFO‑corrected mapping debug (`[hdr-gnu-cfo]`) yields sequences far from GR as expected (diagnostic only).
  - JSON result remains `{ success:false, step:109, reason: header_crc_failed, header:{len:182, cr:1, crc:true} }`.
- Conclusions:
  - Block‑0 `gnu` is stable and correct across anchors: `13 31 15 0 27 0 6 13`.
  - Block‑1 `gnu` varies with (off1,samp1) but never matches GR’s known `GNU1 [0,0,0,22,12,1,5,14]`; Fine‑CFO alone is insufficient.
  - Next: run guided per‑symbol micro‑alignment using `LORA_HDR_GNU_B1="0,0,0,22,12,1,5,14"` to select symbol‑wise shifts that directly target GR’s `gnu`; keep JSON clean and inspect `[hdr-guided] target/chosen` lines.

### 2025-09-13 — Guided per‑symbol alignment (wide micro + slope)

- Run: guided Block‑1 per‑symbol selection with micro (±N/128, ±N/64, ±N/32, ±N/16, ±N/8), slope∈{0,±N/128,±N/64,±N/32,±N/16,±N/8}, and fine‑CFO probes (0, ±1/8, ±1/4 bin) while keeping Block‑0 fixed.
- Result highlights (stderr):
  - Repeated `[hdr-gnu]` baselines around anchors (e.g., off1∈{-3..+7}, samp1∈{−68..+68}) show Block‑1 gnu sequences that remain far from GR targets.
  - `[hdr-guided] target: 0 0 0 22 12 1 5 14 | chosen: ...]` never achieves a full match; best cases only align 1–2 symbols.
  - JSON still ends with `{ "success": false, "reason": "header_crc_failed" }`.
- Interpretation:
  - No qualitative improvement: Block‑0 correct; Block‑1 still off. The failure persists across wide anchor, micro, slope, and small fractional CFO adjustments.
  - The evidence points to residual timing/phase drift within Block‑1 that our current bounded corrections do not model precisely.

### Next experiment — Block‑1 “elimination” toggles (diagnostic)

Goal: Run an automated offline experiment that rebuilds Block‑1 codewords from captured `syms_raw` while toggling specific steps only for Block‑1 to observe sensitivity:
- Toggle Gray mapping for Block‑1 (standard vs no‑gray).
- Toggle header CRC check (report bytes both with/without CRC validation).
- Toggle whitening (N/A for header, included to confirm no effect).

Tooling: add `scripts/block1_elimination_scan.py` which will:
- Run a narrow `--hdr-scan` (fine radius) at the best known timing to capture `syms_raw`.
- Reconstruct CW bytes and header bytes with the toggles above (Block‑0 always standard).
- Compare Block‑1 CW bytes against GR’s ground truth for the canonical vector and emit a JSON report plus terminal progress.


### 2025-09-13 — Auto header phase search (anchor grid) run and findings

- Run:
  - cmd:
    - `python3 scripts/auto_hdr_phase_search.py --exe build/lora_decode --vec vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown --sf 7 --cr 45 --sync 0x12 --min-pre 8 --timeout 300 --anchor-timeout 20 --out logs/auto_hdr_phase.json`
  - progress: heartbeat and per‑anchor lines printed (`[auto] start …`, `… still running`, `ok=False`).

- Result:
  - `logs/auto_hdr_phase.json`:
    ```
    { "ok": false, "best": { "match": -1, "rot": null, "so": null, "sa": null }, "reason": "exhausted" }
    ```
  - All 105 anchors were attempted; no header CRC pass was found within the grid `sym_off∈{-2..2}`, `samp_off∈{-80..80, step 8}` with `LORA_HDR_BLOCK_CFO=2`, `MICRO=1`, `SLOPE=1`, `EPS_WIDE=1` enabled.
  - `match=-1` across anchors indicates no `[hdr-guided]` metrics were captured (we did not set `LORA_DEBUG=1`, which gates those stderr lines).

- Conclusions:
  - The base anchor grid alone did not yield a valid header; consistent with prior evidence that Block‑1 needs finer per‑symbol timing/phase alignment than coarse anchor shifts provide.
  - To diagnose guided alignment quality per anchor, rerun with `LORA_DEBUG=1` (stderr‑only) to collect `[hdr-guided] target(rot=…,match=…)` for each attempt.

- Next steps:
  1) Run index‑trace to verify exact symbol start arithmetic:
     - `LORA_HDR_INDEX=1 build/lora_decode --in vectors/... --sf 7 --cr 45 --sync 0x12 --min-preamble 8 --json 2> logs/hdr_index.err`
     - Expect: `b1_raw - b0_raw == 8*N*OS` and 16 starts spaced by `N*OS`.
  2) Re‑run auto phase search with guided metrics:
     - `LORA_DEBUG=1 python3 scripts/auto_hdr_phase_search.py ... --anchor-timeout 12 --timeout 1800 > logs/auto_hdr_phase.term 2>&1`
     - Inspect best `(match,rot)` per anchor; focus follow‑ups on anchors with highest matches.


### 2025-09-14 — Fractional‑bin diagnostics and Block‑1 compensation attempts

- What we added in code (guarded by env flags):
  - `LORA_HDR_FRAC_LOG=1`: per‑symbol parabolic peak interpolation δ (fractional bin) and ν (phase slope in cycles/sample) after dechirp.
  - `LORA_HDR_HANN=1`: optional Hann window before FFT to reduce leakage.
  - `LORA_HDR_FD1=1` (+ `LORA_HDR_FD1_WIDE`, `LORA_HDR_FD_GAIN`, `LORA_HDR_FD_SLOPE_GAIN`): Block‑1 fractional‑delay (FD) sweep before dechirp.
  - `LORA_HDR_SYM_FCFO=1`: per‑symbol fractional CFO compensation derived from measured δ.

- Runs (anchors near best):
  - `so=-1, sa=72`; also checked nearby anchors.
  - Logs: `logs/fd_diag_so-1_sa72.term`, `logs/fd_fix_so-1_sa72.term`, `logs/sfcfo_*.term`.

- Key observations from `[hdr-frac]` and `[hdr-block-cfo]`:
  - Block‑0: δ is small and stable (≈ −0.03…+0.14; typically ≤0.03). ν is moderate (|ν|≲0.48) with weak sign alternation.
  - Block‑1: δ is large and inconsistent (many around ±0.30…±0.48, often flipping sign symbol‑to‑symbol). ν is large (≈0.28…0.49) with sign changes. This is clear evidence of intra‑block micro‑timing/SFO.
  - Block‑wise CFO: `eps1` is small (≈0.0078…0.02 cycles/sample), so a constant CFO does not explain Block‑1’s large δ.

- FD attempt (`[hdr-fd]`):
  - Swept constant delays of ±0.25/±0.5 samples and a small slope (derived from Block‑0 δ), with/without Hann.
  - Produced various `g1` sequences but no header CRC pass; no convergence toward GR values observed.

- Per‑symbol fractional CFO (`[hdr-sfcfo]`):
  - Applied `ε[s] ≈ −δ[s]/N` per Block‑1 symbol (both signs tried).
  - `g1` changed significantly but did not converge to GR targets; still `header_crc_failed`.

- What this means (focus):
  - Not a constant CFO or anchor/OS issue. It is within‑Block‑1 timing drift (SFO/fractional‑delay) causing large fractional‑bin bias and symbol‑to‑symbol inconsistency.
  - A constant or small linear FD is insufficient; we likely need per‑symbol sub‑sample compensation (a short “time‑warp”) along the block, derived directly from Block‑1’s δ.

- Next plan (concrete):
  1) Compute `δ[s]` in Block‑1 and apply per‑symbol FD before dechirp: `fd[s] = k·δ[s]` with `k ∈ {±0.5, ±1.0}` samples (short sweep), combined with Hann.
  2) Combine sFCFO+FD: `ε[s] = −δ[s]/N` together with `fd[s]` to correct both phase and timing.
  3) Re‑measure δ/ν after compensation; criteria: drive Block‑1 |δ| to ≲0.05 and increase Hamming‑valid CWs prior to header CRC.
  4) If needed, add a small chirp‑rate tweak (SFO) across the 8 symbols.

- Success criteria (unchanged): Header CRC pass, and 10/10 CW equal to GR for both blocks.

• Follow-up run (FD gain sweep):
  - Command: `LORA_HDR_FD1=1 LORA_HDR_FD1_WIDE=1 LORA_HDR_FD_GAIN=2.0 LORA_HDR_FD_SLOPE_GAIN=2.0 LORA_HDR_BLOCK_CFO=2 LORA_HDR_HANN=1 LORA_HDR_FRAC_LOG=1 LORA_DEBUG=1 LORA_HDR_BASE_SYM_OFF=-1 LORA_HDR_BASE_SAMP_OFF=72`
  - Files: `logs/fd_fix_so-1_sa72_gain2.term`, `logs/fd_fix_so-1_sa72_gain2.json`
  - Result: JSON shows `success=false`, `reason=header_crc_failed`, `detect_os=2`. No header CRC pass.
  - Note: `[hdr-fd]` g1 sequences vary across const/slope, but no convergence to GR CW; consistent with prior conclusion that per‑symbol timing warp is required.

### 2025-09-14 — Auto‑warp resampling, guided locks, and 2D mu+eps search

- What we added in code (guarded by env flags):
  - `LORA_HDR_FD_FORCE_MU`: force an 8‑value mu vector (samples) per Block‑1 symbol; resample pre‑dechirp with Lagrange (order configurable by `LORA_HDR_RESAMP_ORDER`).
  - `LORA_HDR_AUTO_WARP`: compute mu from measured δ/ν per symbol (optional median smoothing; gains `LORA_HDR_AUTO_WARP_GAIN`, `LORA_HDR_AUTO_WARP_KNU`). Logs `[hdr-auto-warp]`.
  - `LORA_HDR_FD_SEARCH`: per‑symbol mu grid search with optional “sharpness” metric (`LORA_HDR_FD_SRCH_SHARP=1`) to maximize FFT peak contrast before dechirp.
  - `LORA_HDR_FD_LOCK_GNU`: guided lock of mu per symbol to maximize magnitude at GR target bins (`LORA_HDR_FORCE_GNU_B1="0,0,0,22,12,1,5,14"`). Logs `[hdr-fd-lock]`.
  - `LORA_HDR_FD_LOCK_2D`: guided 2D search (per‑symbol mu + small per‑symbol CFO `eps`) to target bins; logs `[hdr-fd-lock-2d]`.

- Runs and results (highlights):
  - Auto‑warp (order=11): `logs/auto_warp_o11{.term,.json}` → `success=false (header_crc_failed)`; `[hdr-auto-warp]` shows mu applied; Block‑1 g1 changes but no CRC.
  - Auto‑warp (kd=2.0, knu=0.3): `logs/auto_warp_o11_kd2_kn03{.term,.json}` → still fails; δ/ν remain large/inconsistent.
  - FD per‑symbol resampler ± sFCFO: `logs/fd_per_resamp{,_fcfo}{.term,.json}` → no CRC; g1 varies.
  - FD search (sharpness): `logs/fd_search_sharp.json` → adjusted mu but no CRC.
  - Guided lock to GR bins (order=11/21, fine mu): `logs/fd_lock_gnu{,_o21_fine}{.term,.json}` and parity variants `sa=71/73` → all `header_crc_failed`.
  - Guided 2D lock (mu+eps): `logs/fd_lock_2d{.term,.json}` → still `header_crc_failed`.

- Key observations from guided logs:
  - `[hdr-fd-lock]` and `[hdr-fd-lock-2d]` produce large g1 swings across mu/eps patterns. The timing lever is active, but Block‑1 bins do not settle on GR targets across all 8 symbols.
  - Block CFO remains small and steady; parity (sa=71/73) does not help. Root cause is intra‑symbol fractional delay/SFO in Block‑1, not CFO or base indexing.

- Conclusion:
  - Time‑domain compensation is necessary but our current per‑symbol mu warps (even with per‑symbol ε) are not sufficient to achieve a consistent GR‑like g1 across all symbols. Evidence points to symbol‑rate mismatch (SFO) and residual chirp‑slope error within Block‑1 that require retiming symbol boundaries and/or matched dechirp slope per symbol.

- Next steps (focused, high‑signal):
  1) Symbol‑boundary SFO tracking: estimate accumulated fractional sample drift across Block‑1 from δ trend; retime Block‑1 symbol starts (`idx1 + s*N + Δs`) instead of (or in addition to) resampling the waveform. Log `Δs[s]`, residual δ/ν.
  2) Chirp‑slope matched dechirp: derive a small per‑symbol slope tweak from ν to cancel residual phase slope during dechirp (ν→effective SFO); measure post‑compensation δ/ν.
  3) Smoothness prior across symbols: when searching mu/eps, enforce a small smoothness constraint across s=0..7 to avoid per‑symbol overfitting; prefer linear/quadratic drift models.
  4) Success condition unchanged: drive Block‑1 median |δ| ≲ 0.05 and achieve header CRC pass with 10/10 CWs matching GR.


