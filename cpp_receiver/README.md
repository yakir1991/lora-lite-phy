# LoRa C++ Receiver (Reboot)

This directory hosts the fresh C++ implementation that mirrors the proven Python `external/sdr_lora` pipeline. The rebuild proceeds stage-by-stage, validating each component against the Python reference before adding the next element.

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

1. **Expand vector coverage:** generate and validate frames across SF9–12, CR3–4, implicit headers, CRC-off cases, and alternate sync words/sample rates.
2. **Automated parity suite:** plug the new vectors into regression tests (Python + C++ CLI) so every configuration is exercised automatically.
3. **Performance & profiling:** once coverage is complete, profile the hot loops (FFT, dewhitening, CRC) and optimise as needed.

Each milestone will add dedicated tests that compare numerical outputs against Python dumps produced from `external/sdr_lora` to guarantee parity as we progress toward a fully native C++ decoder.
