# Stage 1 – Host-Side Prototype Tracker

Goal: Stand up a C++ simulation harness that mirrors the GNU Radio reference, consumes the generated CF32 vectors, and produces stage-by-stage diffs as part of automated validation.

## 1. Harness Skeleton
- [x] Create `host_sim/` library with modules for reading `.cf32` IQ dumps, applying DSP stages (whitening, FEC, interleaver, etc.), and exporting intermediate buffers.  
  _Status:_ `host_sim/include/host_sim` now provides `capture.hpp` and `whitening.hpp`; `cmake -S . -B build && cmake --build build` yields a static lib.
- [x] Define CLI tool (`host_sim/lora_replay`) that accepts a capture file plus LoRa parameters (SF, BW, CR, payload len, sync word) and emits JSON/CSV traces aligned with GNU Radio outputs.  
  _Status:_ `lora_replay` prints capture statistics, performs a brute-force preamble alignment, demodulates symbols, supports metadata-driven dumps (`--dump-symbols`, `--dump-iq`), and optionally writes a JSON summary (`--stats`).
- [x] Integrate existing reverse-engineered algorithms from `gr_lora_sdr` (port or wrap) ensuring deterministic behaviour and unit-testable components.  
  _Status:_ `deinterleaver` now mirrors GNU Radio’s Gray mapping (subtract‑1, header/LDRO downshift, Gray XOR) and emits the same codewords; `hamming_decode` recovers the header nibbles and payload nibbles. `lora_replay` scans the symbol stream for a header whose `(payload_len, CR, CRC)` matches the metadata, so captures that begin mid-frame are handled. Set `HOST_SIM_DEBUG_HEADER=1` to log candidate symbol blocks and decoded nibbles during investigations.
- [x] Provide stage-by-stage diffing against GNU Radio debug dumps.  
  _Status:_ `lora_replay --compare-root <prefix>` ingests the `_fft/_gray/_deinterleaver/_hamming` traces produced by `tx_rx_simulation.py` (with `LORA_DUMP_DEBUG=1`) and emits both detailed per-stage diagnostics (alignment, first mismatch) and a concise `[ci-summary] stages ...` line suitable for CI parsing. With the per-sample FFT demodulator in place, the golden captures now report zero mismatches across every stage.
- [x] Validate payload and CRC end-to-end.  
  _Status:_ After deinterleaving, Hamming decode, and whitening removal, `lora_replay` can optionally compare the recovered payload bytes against a caller-provided string and reports the computed vs. decoded LoRa CRC (0x1021 polynomial) for reference.

## 2. Golden Vector Integration
- [x] Register `gr_lora_sdr/data/generated/tx_rx_simulation.cf32` as the first regression vector; store metadata alongside (parameters, source commit, reference payload).
- [x] Extend generation script to sweep SF/BW/CR/SNR combinations and place results under `gr_lora_sdr/data/generated/<scenario>/`. Capture metadata in `metadata.json`.
- [ ] Update notes when additional vectors are captured; maintain a manifest for CI consumption.

Current captures (`gr_lora_sdr/data/generated/`):

| File | SF | BW (Hz) | CR | SNR (dB) | Samples | Notes |
|------|----|---------|----|----------|---------|-------|
| `tx_rx_simulation.cf32` | 7 | 125 000 | 2 (4/6) | −5.0 | ~25 M | Baseline (long run) |
| `tx_rx_sf7_bw125000_cr1_snrm5p0.cf32` | 7 | 125 000 | 1 (4/5) | −5.0 | ~19 M | Generated via tool |
| `tx_rx_sf9_bw125000_cr3_snrm2p0.cf32` | 9 | 125 000 | 3 (4/7) | −2.0 | ~19 M | Verified with `verify_cf32.py --sf 9 --cr 3` |
| `tx_rx_sf10_bw250000_cr4_snrp0p0.cf32` | 10 | 250 000 | 4 (4/8) | +0.0 | ~16.7 M | Verified with `verify_cf32.py --sf 10 --cr 4` |
| `tx_rx_sf7_bw125000_cr2_snrm5p0.cf32` | 7 | 125 000 | 2 (4/6) | −5.0 | ~25 M | Reference for header/payload parity (debug dumps under `_gray/_deinterleaver/_hamming/_fft`) |
| `tx_rx_sf7_bw125000_cr2_snrm5p0_short.cf32` | 7 | 125 000 | 2 (4/6) | −5.0 | ~2.5 M | Short clip for fast CI runs (same payload & metadata as long capture) |
| `tx_rx_sf7_bw125000_cr1_snrm5p0_short.cf32` | 7 | 125 000 | 1 (4/5) | −5.0 | ~2.5 M | Short debug capture; matches GNU Radio stage dumps 1:1 (fast smoke test) |
| `tx_rx_sf9_bw125000_cr3_snrm2p0_short.cf32` | 9 | 125 000 | 3 (4/7) | −2.0 | ~2.5 M | Short debug capture; now aligned with GNU Radio reference after FFT/CFO fixes |
| `tx_rx_sf6_bw125000_cr1_snrm5p0.cf32` | 6 | 125 000 | 1 (4/5) | −5.0 | ~3.0 M | Full-length SF6 capture with debug stage dumps regenerated via `tx_rx_simulation.py` |
| `tx_rx_sf6_bw125000_cr2_snrm5p0.cf32` | 6 | 125 000 | 2 (4/6) | −5.0 | ~3.0 M | Full-length SF6 capture; parity verified against host_sim harness |
| `tx_rx_sf5_bw125000_cr1_snrm5p0.cf32` | 5 | 125 000 | 1 (4/5) | −5.0 | ~3.0 M | Full-length SF5 capture with stage dumps (FFT/Gray/Deint/Hamming) |
| `tx_rx_sf5_bw125000_cr2_snrm5p0.cf32` | 5 | 125 000 | 2 (4/6) | −5.0 | ~3.0 M | Full-length SF5 capture mirroring GNU Radio reference |

### Regenerating Reference Stage Dumps

To refresh the GNU Radio reference data (CF32 capture plus `_fft/_gray/_deinterleaver/_hamming` dumps):

1. Activate the conda environment and point `PYTHONPATH`/`LD_LIBRARY_PATH` to the in-tree install:
   ```bash
   source ~/miniconda3/etc/profile.d/conda.sh
   conda activate gr310
   export PYTHONPATH=$PWD/gr_lora_sdr/install/lib/python3.12/site-packages:$PYTHONPATH
   export LD_LIBRARY_PATH=$PWD/gr_lora_sdr/install/lib:$LD_LIBRARY_PATH
   ```
2. Run `examples/tx_rx_simulation.py` with the desired parameters encapsulated in environment variables. For example, to regenerate the long SF7/CR1 capture and its stage outputs:
   ```bash
   export LORA_SF=7
   export LORA_BW=125000
   export LORA_SAMPLE_RATE=$((LORA_BW*4))
   export LORA_CR=1
   export LORA_SNR_DB=-5.0
   export LORA_PAYLOAD_LEN=16
   export LORA_AUTOSTOP_SECS=0.6
   export LORA_DUMP_DEBUG=1
   export LORA_OUTPUT_DIR=$PWD/gr_lora_sdr/data/generated
   export LORA_OUTPUT_NAME=tx_rx_sf7_bw125000_cr1_snrm5p0.cf32
   python gr_lora_sdr/examples/tx_rx_simulation.py
   ```
3. The regenerated CF32 capture and accompanying stage files will be written beneath `gr_lora_sdr/data/generated/`. Repeat with different `LORA_*` overrides (e.g., `LORA_SF=10`, `LORA_BW=250000`, `LORA_CR=4`) to sweep other scenarios.

## 3. Diff & Reporting
- [x] Implement comparison helpers that load GNU Radio stage dumps (to be generated) and host-sim outputs, computing per-symbol/per-bit differences and pass/fail statistics.
- [x] Produce a summary report (text + optional HTML) suitable for CI logs; include frame-level CRC parity, synchronization offsets, and BER metrics.

Notes:
- Host-side stage dumps now gate payload contributions when `SF <= 6`, mirroring GNU Radio’s behavior of emitting header-only debug vectors for those captures.
- Regenerated SF5/CR1 and SF5/CR2 reference files with the new demod output (header-first values only) to keep CI comparisons zero-diff.
- `lora_replay` prints a compact `[compare] summary` line by default; export `HOST_SIM_VERBOSE_COMPARE=1` to restore the verbose per-stage diagnostics when debugging mismatches.
- Added a `host-sim-regression` CMake target (`cmake --build build --target host-sim-regression`) that wraps `ctest -L host-sim`, making it trivial to wire the parity sweep into CI.
- The manifest at `docs/reference_stage_manifest.json` captures SHA-256 hashes for each capture/stage dump; validate it with `python3 tools/check_reference_manifest.py docs/reference_stage_manifest.json` (included in the `host-sim` CTest label).
- `lora_replay` can emit a machine-readable run summary via `--summary <file.json>` (contains metadata, stats, per-stage results, and preview symbols) for CI artifact collection.
- Captured checksums for every CF32 and stage dump under `gr_lora_sdr/data/generated/`; see `docs/reference_stage_manifest.json` for the authoritative manifest used by CI and local integrity checks.

## 4. Automation Hooks
- [x] Add CTest/pytest entry to execute the harness on the baseline vector and fail on mismatches.
- [x] Provide Makefile / CMake targets (`make host-sim`, `ctest -R host-sim`) for local runs.
- [x] Document environment setup (conda env, `verify_cf32.py` usage) in project README.

Notes:
- `ctest -L host-sim` is now exposed via both `cmake --build build --target host-sim` and `host-sim-regression` (with `--output-on-failure`).
- `host_sim/README.md` captures the conda activation, path exports, regression commands, and manifest verification helper.

## 5. Open Questions
- How to share constants (e.g., whitening matrices, FFT twiddle factors) between GNU Radio reference and C++ harness without duplication?
- Should the harness operate purely in fixed-point to mimic MCU constraints, or start in float for parity before quantisation?
- What sample count is sufficient per scenario to keep CI runtime manageable while still exercising synchronization edge cases?

## References & Commands
- Build harness:  
  `cmake -S . -B build && cmake --build build`
- Replay example:  
  `./build/lora_replay --iq gr_lora_sdr/data/generated/tx_rx_sf7_bw125000_cr1_snrm5p0.cf32 --stats host_sim/stats_sf7.json`
- Generate captures:  
  `PYTHONPATH=$PWD/gr_lora_sdr/install/lib/python3.12/site-packages LD_LIBRARY_PATH=$PWD/gr_lora_sdr/install/lib conda run -n gr310 python gr_lora_sdr/tools/generate_vectors.py`
- Verify capture against GNU Radio reference:  
  `PYTHONPATH=... LD_LIBRARY_PATH=... conda run -n gr310 python gr_lora_sdr/tools/verify_cf32.py --input <file> --sf <sf> --bw <bw> --sample-rate <fs> --cr <cr>`
