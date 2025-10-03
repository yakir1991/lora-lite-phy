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
- **Stage tests** (`run_stage_tests`): CMake target that covers loader, frame sync, header decode, payload decode, and sync analysis against the reference vector so regressions are caught immediately.

## Next steps

1. **CLI harness:** add a small executable/CLI that calls the `Receiver` façade so it can be used in tooling, regression tests, or compared directly with the Python decoder.
2. **Performance and robustness:** broaden test coverage (different CR/SF/LDRO combos) and micro-optimise the matched filters/FFTs once correctness is locked.

Each milestone will add dedicated tests that compare numerical outputs against Python dumps produced from `external/sdr_lora` to guarantee parity as we progress toward a fully native C++ decoder.
