# Offline decode investigation

## Summary
- Running the offline decode helper against captures produced by GNU Radio (`python3 scripts/decode_offline_recording_final.py <capture>`)
  triggered the new C++ pipeline and consistently stopped after the header stage with debug output reporting that frame 0 was
  "missing payload symbols"; the script then surfaced a failed decode with no payload bytes.【F:scripts/decode_offline_recording_final.py†L75-L175】【F:src/rx/gr_pipeline.cpp†L414-L427】
- The multi-frame decoder in `src/rx/gr_pipeline.cpp` assumed a fixed payload symbol budget instead of deriving it from the decoded
  header. Any capture whose payload was shorter than the hard-coded count left the loop believing the payload symbols were
  truncated, so it aborted without returning user data.【F:src/rx/gr_pipeline.cpp†L348-L429】

- Computing the expected number of payload symbols from the header fields (payload length, coding rate, CRC flag) unblocks the decoder for any valid payload length and restored the Python wrapper to working order.【F:src/rx/gr_pipeline.cpp†L28-L47】【F:src/rx/gr_pipeline.cpp†L348-L356】
- Follow-up verification initially stalled because the container lacked `liquid-dsp`; CMake now falls back to the vendored `external/liquid-dsp` sources when the system library is absent so future rebuilds of `test_gr_pipeline` can proceed inside clean environments once `pybind11` is also available.【F:CMakeLists.txt†L8-L55】
- Relaxed the offline decode helper so it accepts hexadecimal header fields, letting the investigation surface the 242-byte payload (CRC currently invalid) instead of crashing on the debug dump.【F:scripts/decode_offline_recording_final.py†L20-L175】【a85798†L1-L9】
- Re-initialised the vendored `external/liquid-dsp` submodule and rebuilt the pipeline inside the container so `test_gr_pipeline` can run; the debug run still reports `CRC calc=699c` vs `CRC rx=5e6d` for the 242-byte frame, confirming the payload bits look plausible but the CRC stage diverges.【790c15†L1-L4】【7f750c†L1-L17】【f93ffd†L1-L2】【3a6e3f†L23-L36】
- Re-ran the Python harness against the capture and confirmed the C++ CRC engine reproduces the `0x699c` checksum on the 242-byte payload while the on-air trailer still reads `0x5e6d`; recomputing the checksum with the captured trailer and sweeping whitening offsets leaves the mismatch unchanged, so the corruption must stem from an earlier demodulation/FEC stage rather than CRC parameters or dewhitening.【a8c3a5†L1-L10】【bfd100†L1-L2】【675f51†L1-L1】【7843c7†L1-L8】
- Feeding the higher-rate capture `sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown`
  exposed a second failure mode: the header advertises a 96-byte payload, but only ~18k baseband samples are present so the
  multi-frame loop aborted before producing payload bytes.【ba19bc†L1-L24】 Adding a fallback that clamps the payload symbol
  budget to the available samples and truncates the CRC expectation allows the helper to recover two partial frames (94 and
  26 bytes) even though their CRCs remain invalid and the decoded text is still garbage, confirming the underlying FEC
  corruption persists.【F:src/rx/gr_pipeline.cpp†L367-L383】【F:src/rx/gr_pipeline.cpp†L614-L719】【83e45b†L1-L52】【00689f†L1-L5】
- Updated the diagonal interleaver map to mirror GNU Radio’s `mod((i - j - 1), sf)` rotation, but the 500 ksps capture still
  demaps to repeating garbage (`1b 27 29 …`) and a failed CRC, so the corruption predates deinterleaving.【F:src/rx/gr/utils.cpp†L24-L44】【8faf71†L1-L7】


## Root cause analysis
`decode_offline_recording_final.py` shells out to the `test_gr_pipeline` executable, parses the diagnostic stdout, and exposes the
payload bytes and CRC state back to Python callers.【F:scripts/decode_offline_recording_final.py†L75-L155】【F:scripts/decode_offline_recording_final.py†L168-L209】
When the C++ pipeline processed an IQ file whose payload was shorter than the assumed constant, the multi-frame loop demodulated
all available symbols but then compared the observed payload symbol count against the hard-coded expectation. Because the
comparison always failed, the decoder logged `Frame 0 missing payload symbols`, treated the frame as incomplete, and stopped
short of the FEC/whitening stages.【F:src/rx/gr_pipeline.cpp†L414-L427】 This left the Python helper with a "pipeline failed"
condition even though the samples contained a valid LoRa frame.

## Corrective actions
1. Added `expected_payload_symbols` to compute the per-frame payload symbol count from the decoded header, spreading factor,
   and LDRO setting so the logic mirrors the GNU Radio reference exactly.【F:src/rx/gr_pipeline.cpp†L28-L47】
2. Replaced the hard-coded constant inside the multi-frame loop with the computed value so the demodulator now accepts any
   payload length permitted by the header fields before forwarding symbols to the interleaver, FEC, and whitening stages.【F:src/rx/gr_pipeline.cpp†L348-L356】【F:src/rx/gr_pipeline.cpp†L419-L437】


## Verification status

- Reconfigured the tree with `cmake -S . -B build -Dpybind11_DIR=/root/.local/lib/python3.12/site-packages/pybind11/share/cmake/pybind11` after initialising `external/liquid-dsp`; the configure step now completes and `cmake --build build` produces `test_gr_pipeline` alongside the Python extension.【434ab2†L1-L7】【06b1db†L1-L2】
- Running `python3 scripts/decode_offline_recording_final.py vectors/sps_125k_bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_nmsgs_8.unknown` now decodes one frame and prints the 242-byte payload with a failing CRC, confirming the parser fix and highlighting the remaining CRC mismatch.【a85798†L1-L9】
- Running the same helper against `sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown`
  yields two truncated frames (94 and 26 bytes) with failing CRCs and non-sensical text, demonstrating that the new clamping
  logic prevents the hard failure but the payload bits are still mangled upstream.【83e45b†L1-L52】【00689f†L1-L5】
- Invoking `./build/test_gr_pipeline` against the same capture prints the Hamming-decoded nibbles, dewhitened payload bytes, and the mismatched CRC pair (`calc=699c`, `rx=5e6d`), giving concrete data for the next debugging step.【7adb98†L1-L18】【3a6e3f†L1-L36】【3cf043†L1-L19】
- Verified with a standalone Python script that the pipeline’s CRC calculator still yields `0x699c` when run over the dewhitened payload bytes and that appending the captured CRC (`0x5e6d`) does not zero the remainder; rotating the whitening sequence at the nibble level across eight offsets also leaves both values unchanged, eliminating simple CRC or whitening misconfiguration as causes.【bfd100†L1-L2】【675f51†L1-L1】【7843c7†L1-L8】

## Next steps

- Investigate why the pipeline reports mismatched CRC values (`CRC calc=699c`, `CRC rx=5e6d`) on the sample capture by tracing back through the payload demodulation stack—deinterleaving, Hamming decode, and nibble assembly—to spot the corruption that survives CRC recomputation despite matching whitening and polynomial settings.【3a6e3f†L23-L36】【7843c7†L1-L8】
- Extend that investigation to the 500 ksps capture: even with payload clamping and the corrected diagonal interleaver, the
  demapper reports payload symbols such as `natural=124,1,110,109,22` for the first block instead of the sequence produced by
  encoding `hello_stupid_world`, indicating the corruption occurs before FEC decode—likely in the FFT/Gray demap or CFO/STO
  compensation stages.【F:src/rx/gr_pipeline.cpp†L519-L575】【acff51†L16-L58】
- After the CRC mismatch is resolved, rerun the offline decode regression end-to-end and capture the successful output in this document to close the investigation and guard against future regressions.


## Open follow-ups
- The pipeline still relies on the caller to seed `Config.header_symbol_count` (currently set to 16 in `test_gr_pipeline`). We should
  move this derivation inside the pipeline so Python bindings or future tools cannot forget to initialise it.【F:test_gr_pipeline.cpp†L26-L40】【F:src/rx/gr_pipeline.cpp†L349-L420】
- Add regression coverage that runs the C++ pipeline across multiple payload lengths via the Python harness, ensuring the JSON
  parsing path exercised by `decode_offline_recording_final.py` stays healthy.【F:scripts/decode_offline_recording_final.py†L75-L209】
- Convert the verbose debug prints that surfaced during this investigation (`Frame 0 missing payload symbols`, FFT dumps, etc.)
  into structured logging so future triage can be filtered more easily.【F:src/rx/gr_pipeline.cpp†L352-L440】

## Related files
- Offline decode script: `scripts/decode_offline_recording_final.py`
- GNU Radio compatible pipeline implementation: `src/rx/gr_pipeline.cpp`
- Standalone CLI used by the script: `test_gr_pipeline.cpp`
