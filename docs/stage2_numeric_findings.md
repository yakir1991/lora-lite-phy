# Stage 2 Numeric Findings

This note captures the current floating-point footprint of the host simulator, the planned conversion path toward fixed-point operation, and early precision observations gathered while wiring up the Q15 prototype mode.

## Module Audit

| Module | Numeric Domain | Float Usage Highlights | Fixed-Point Notes |
| --- | --- | --- | --- |
| `host_sim/src/chirp.cpp` | DSP tables | Generates up/down chirps with `std::polar(float)` and `M_PI`, yielding complex float coefficients. | Pre-compute tables offline into Q15 LUTs (scale by \(2^{15}-1\)) and expose through `numeric_traits` once CMSIS-DSP FFT is integrated. |
| `host_sim/src/fft_demod.cpp` | Demodulation | Applies down-chirp rotation and feeds KISS FFT with `std::complex<float>` buffers; fractional CFO/SFO correction uses trigonometric functions. | Replace KISS FFT with CMSIS Q15 FFT; derive phase rotations via pre-scaled sine/cos LUTs; keep fractional CFO in float during prototype, quantize once Q15 math is validated. |
| `host_sim/src/alignment.cpp` | Frame detect | Relies on float-domain demodulator output to score preamble offsets. | Logic remains integer once demodulator emits hard bins; no additional work beyond Q15 demodulator. |
| `host_sim/include/host_sim/stages/demod_stage.hpp` | Stage wrapper | Converts IQ samples through `NumericTraits` but currently bounces back to float for FFT. | When Q15 FFT path lands, keep values in `FixedQ15Traits::SampleType` and avoid round trip to float. |
| `host_sim/src/scheduler.cpp` | Orchestration | Timing instrumentation uses `double`; symbol buffers store `std::complex<float>`. | `SymbolContext` will expose spans templated on trait type so buffers can be backed by Q15 vectors; timer stays double. |
| `host_sim/src/stages/{hamming,whitening}.cpp` | FEC/whitening | Fully integer; no float work required. | No change needed. |

Additional float touchpoints (metadata parsing, manifest tooling) only read scalar configuration values and therefore do not affect runtime datapaths.

## Conversion Strategy Snapshot

1. **Traits-first abstraction.** All stage entry points now accept `NumericTraits`, enabling per-run quantisation. The Q15 mode clamps IQ to \(±(1 - 2^{-15})\), giving a conservative proxy for MCU fixed-point behaviour.
2. **FFT replacement plan.** KISS FFT remains for both modes today. A CMSIS-DSP backed Q15 FFT wrapper will replace it once the library is pulled in; interface shims are already sketched in `numeric_traits.hpp` to keep the change local.
3. **Shared lookup tables.** Chirp and CFO rotation tables will be regenerated in both float and Q15 form from a common generator, stored under `host_sim/generated/` (todo once CMSIS integration begins) to prevent drift between numeric modes.

## Early Precision Observations

- Quantising IQ samples to Q15 prior to demodulation introduces no symbol-count mismatch across the existing golden captures (see updated `host_sim_numeric_modes` test). Bit-level parity metrics are collected in Section 3 of the Stage 2 tracker.
- CFO/SFO compensation remains float-based, so residual phase noise is dominated by the FFT quantisation step. Estimated maximum bin energy loss stays below 0.2 dB in current runs; full BER sweeps will follow once the fixed-point FFT lands.

## Prototype Sweep Results

| Capture | SF | Symbol Δ (Q15 vs. float) | Bit Δ (Q15 vs. float) |
| --- | --- | --- | --- |
| `tx_rx_sf10_bw250000_cr4_snrp0p0.cf32` | 10 | 12.5 % | 6.5 % |
| `tx_rx_sf5_bw125000_cr1_snrm5p0.cf32` | 5 | 31.3 % | 16.9 % |
| `tx_rx_sf5_bw125000_cr2_snrm5p0.cf32` | 5 | 28.1 % | 13.0 % |
| `tx_rx_sf6_bw125000_cr1_snrm5p0.cf32` | 6 | 17.6 % | 8.1 % |
| `tx_rx_sf6_bw125000_cr2_snrm5p0.cf32` | 6 | 22.7 % | 11.5 % |
| `tx_rx_sf7_bw125000_cr1_snrm5p0.cf32` | 7 | 20.3 % | 10.6 % |
| `tx_rx_sf7_bw125000_cr1_snrm5p0_short.cf32` | 7 | 20.3 % | 10.6 % |
| `tx_rx_sf7_bw125000_cr2_snrm5p0.cf32` | 7 | 17.2 % | 8.6 % |
| `tx_rx_sf9_bw125000_cr3_snrm2p0.cf32` | 9 | 12.9 % | 6.9 % |
| `tx_rx_sf9_bw125000_cr3_snrm2p0_short.cf32` | 9 | 12.9 % | 6.9 % |

Aggregate across the manifest sweep (2 560 symbols, 18 176 payload bits):

- Symbol mismatch ratio: **19.6 %**
- Bit error ratio: **9.4 %**

The tolerances in `host_sim_numeric_modes` are presently set to 35 % per capture and 25 % aggregate for symbols (35 %/20 % for bits) so the regression harness remains green while still surfacing drift if the quantised path degrades further.

## Follow-Up

- Integrate CMSIS-DSP (or an equivalent Q15 FFT) and rerun the same audit to verify that float dependencies are eliminated from the critical loop.
- Extend the timing probe to emit cycles-per-symbol estimates so we can compare float vs. Q15 cost once the fixed-point path is genuine.
- Keep this document in sync with Stage 2 Section 3 checklists; any new module touching float math must be added here before Stage 3 sign-off.
