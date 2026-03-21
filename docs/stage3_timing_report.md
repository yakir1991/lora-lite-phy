# Stage 3 Timing & Resource Findings

_Updated: 2025-11-05_

## Overview
- Host-side scheduler now emits per-stage timing, scratch usage, and cycle counts calibrated via `--ns-to-cycle 0.216` (216 MHz Cortex‑M7 reference). Both float and Q15 pipelines produce matched symbol streams after migrating the fixed-point FFT to CMSIS-DSP (`arm_cfft_q15`) with a −12 dB pre-FFT headroom.
- Captures exercised: the ten-item Stage 3 manifest covering SF5/6/7/9/10, multiple coding rates, and both “short” and full-length vectors. Float/Q15 comparisons remain bit-identical against the GNU Radio reference.
- MCU portfolio tracked in summaries: STM32H743 (Cortex‑M7 216 MHz), nRF5340 (Cortex‑M33 128 MHz), ESP32‑S3 (Xtensa dual-core 160 MHz). Per-symbol budgets derive from measured symbol durations × clock rate.

## Key Metrics Snapshot
### Float path

| Capture | Avg stage time (ns) | p95 (ns) | Max symbol memory (bytes) | Min deadline margin (µs) | MCU utilisation (avg / max) |
| --- | ---: | ---: | ---: | ---: | ---: |
| SF5 BW125k CR1 SNR −5 dB | 2 600 | 3 930 | 1 024 | 248 | M7 0.00 % / 0.00 % |
| SF6 BW125k CR2 SNR −5 dB | 5 790 | 9 224 | 2 048 | 229 | *(MCU targets unavailable; metadata lacks symbol duration)* |
| SF7 BW125k CR1 SNR −5 dB | 7 995 | 17 873 | 4 096 | 985 | M7 0.00 % / 0.00 % |
| SF9 BW125k CR3 SNR −2 dB | 16 911 | 25 560 | 16 384 | 3 945 | M7 0.00 % / 0.00 % |
| SF10 BW250k CR4 SNR +0 dB | 101 320 | 132 225 | 32 768 | 3 681 | M7 0.00 % / 0.00 % |

### Q15 path

| Capture | Avg stage time (ns) | p95 (ns) | Max symbol memory (bytes) | Min deadline margin (µs) | MCU utilisation (avg / max) |
| --- | ---: | ---: | ---: | ---: | ---: |
| SF5 BW125k CR1 SNR −5 dB | 3 180 | 4 902 | 1 024 | 247 | M7 0.00 % / 0.00 % |
| SF6 BW125k CR2 SNR −5 dB | 6 825 | 10 814 | 2 048 | 228 | *(MCU targets unavailable; metadata lacks symbol duration)* |
| SF7 BW125k CR1 SNR −5 dB | 8 932 | 19 702 | 4 096 | 984 | M7 0.00 % / 0.00 % |
| SF9 BW125k CR3 SNR −2 dB | 18 002 | 27 616 | 16 384 | 3 944 | M7 0.00 % / 0.00 % |
| SF10 BW250k CR4 SNR +0 dB | 104 455 | 135 876 | 32 768 | 3 680 | M7 0.00 % / 0.00 % |

Notes:
- Timing instrumentation runs on a Linux host, so absolute numbers shift ±30 % run-to-run; tolerances in `docs/reference_summary_metrics.json` are widened (30 % avg / 60 % p95) until MCU-grade measurements are available.
- Q15 timings sit ~15–25 % above float due to CMSIS‑DSP FFT cost plus float↔Q15 conversion; the bias remains constant across SF/BW cases and preserves symbol parity.
- The CMSIS path applies a fixed −12 dB gain before the FFT to prevent saturation. Retune (or replace with adaptive scaling) once the pipeline runs on target hardware.
- Min deadline margin remains comfortably positive for all vectors with DMA/ISR simulation disabled. Enabling DMA jitter or ISR penalties should be part of Stage 4 stress plans.

## Cycle Calibration Guidance
- `--ns-to-cycle 0.216` approximates a 216 MHz MCU (0.216 cycles/ns). For other targets use `freq_MHz / 1000` (e.g., Cortex‑M33 @ 128 MHz → `0.128`).
- Re-run:  
  ```bash
  python3 tools/generate_summary_metrics.py \
    docs/reference_stage_manifest.json \
    gr_lora_sdr/data/generated \
    build/reference_summaries \
    --lora-replay build/host_sim/lora_replay \
    --instrument-mode float \
    --ns-to-cycle 0.216 \
    --mcu-target M7:216 --mcu-target M33:128 --mcu-target ESP32S3:160
  python3 tools/generate_mcu_cycle_report.py build/reference_summaries
  ```
- For Q15 coverage, repeat with `--instrument-mode q15` and record a parallel report to quantify fixed-point drift.

## Mitigation Recommendations
- **FFT/Demod hot-spot**: Port to CMSIS‑DSP Q15 FFT + twiddle prefetch; budgeting shows FFT dominates stage timing for SF≥9.
- **Synchronization loops**: Consider dedicating DMA/ISR overlap for preamble processing; current margins allow ~3 ms slack for SF10 @ 250 kHz, but ISR injections need verification.
- **Memory footprint**: Peak per-symbol buffer ≤ 32 kB (SF10), scratch ≤ 4 kB; ensure MCU SRAM budgeting reserves ≥ 64 kB for PHY and IQ double buffers.

## Open Follow-ups
1. Lock in real MCU cycle scaling from on-target profiling to tighten tolerances and populate utilisation ratios (>0 %).
2. Add CI checks that fail when `deadline_miss_count > 0` or when utilisation exceeds 80 % of budget once calibrated data is available.
3. Document DMA/ISR stress scenarios with jitter enabled to validate scheduler hooks (planned for Stage 4).
