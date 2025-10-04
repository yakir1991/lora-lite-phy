# LoRa C++ Receiver (Reboot)

This directory hosts an independent C++ LoRa receiver that draws from the proven Python `external/sdr_lora` pipeline and the GNU Radio implementation. Every stage is validated against both reference receivers before it moves forward.

## Current status

- **IQ Loader** (`IqLoader`): reads interleaved `cf32` IQ into `std::vector<std::complex<float>>`. Covered by tests that assert byte-for-byte agreement with NumPy.
- **Preamble Detector** (`PreambleDetector`): matched-filter search using a generated LoRa up-chirp. For the reference vector `sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown` it locks at offset `0` with metric `≈1.0`, matching the Python baseline.
- **Frame Sync** (`FrameSynchronizer`): replicates the GNU Radio-style coarse/fine preamble tracking to recover `p_ofs_est` and CFO (−244.140625 Hz for the reference vector), matching the Python receiver’s internal state.
- **Sync Word Detector** (`SyncWordDetector`): demodulates the eight preamble chirps plus the two sync chirps via a down-chirp FFT, reproducing the symbol bins `[0×8, 8, 16]` and GNU Radio-grade magnitudes seen in the Python receiver. The stage flags any preamble drift or sync-word mismatch.
- **Header Decoder** (`HeaderDecoder`): demodulates the eight explicit-header symbols using the frame-sync alignment, applies the LoRa Gray/interleave/Hamming pipeline, and recovers `length=18`, `CR=2`, `CRC=on` with raw header bins `[90, 122, 122, 126, 18, 110, 22, 78]`, identical to the Python implementation.
- **Payload Decoder** (`PayloadDecoder`): demods all payload symbols, replicates LoRa interleaver/Hamming/whitening, validates the CRC16, and outputs the exact `"hello stupid world"` bytes observed in the Python receiver.
- **Receiver façade** (`Receiver`): simple orchestrator that runs frame sync → header → payload stages and emits the decoded bytes/flags, making it easy to integrate with CLIs or future pipelines.
- **CLI (`decode_cli`)**: command-line decoder that wraps the façade and prints sync/header/CRC status plus payload hex for any `.cf32` file.
- **Stage tests** (`run_stage_tests`): CMake target that covers loader, frame sync, header decode, payload decode, and sync analysis against the reference vector so regressions are caught immediately.

## Tools & scripts

- `cpp_receiver/build/decode_cli`: C++ parity checker (`--sf/--bw/--fs/--ldro` options, `--debug` for extra diagnostics).
- `external/gr_lora_sdr/scripts/export_tx_reference_vector.py`: GNU Radio exporter used to generate golden vectors (SF/CR/LDRO sweeps, explicit/implicit headers, CRC toggles).
- Python reference: `external/sdr_lora/lora.decode` (kept as the oracle for comparison) plus helper scripts in `scripts/` (`sdr_lora_cli.py`, `compare_py_vs_cpp.py`, etc.).
- Golden vectors: new batch in `golden_vectors/new_batch/` (SF7–8, CR1–2, LDRO0/1/2, payload `HELLO_STUPID_WORLD`).

## Next steps

1. **Solidify low-SF support:** fix the SF5/6 header and payload paths (currently crash-prone) and add regression vectors to lock behaviour.
2. **Centralise shared DSP utilities:** extract the duplicated Gray de-mapping, DFT helpers, and timing constants into reusable components shared by all stages.
3. **Broaden automated regressions:** wire the extended golden-vector batches into the C++ stage tests and Python/GNU Radio parity suite so every configuration runs in CI.

With the expanded vector library and existing parity harness in place, these milestones focus on robustness and maintainability as we converge on a production-grade native receiver.
