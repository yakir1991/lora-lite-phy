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
- Running `python3 scripts/decode_offline_recording_final.py vectors/sps_125k_bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_nmsgs_8.unknown` now reaches the pipeline but fails while parsing the stdout because the debug dump prints `Payload length: f2`, which the script tries to convert to an integer.【4fd49c†L1-L2】【ce3066†L19-L48】

## Next steps

- Update either the pipeline logging or the Python parsing to tolerate hexadecimal debug values (e.g., skip the `Payload length: f2` line) so the wrapper can consume the new diagnostics without crashing.【4fd49c†L1-L2】【ce3066†L35-L48】
- Once the parsing issue is resolved, rerun the offline decode regression end-to-end and record the successful output in this document to close the investigation loop and guard against regressions when modifying the payload handling logic again.

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
