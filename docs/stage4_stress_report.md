# Stage 4 Stress Report

Generated via `tools/run_rt_stress.py` with matrix `docs/stage4_rt_matrix.json`.

| Profile | Cases | Max Tracking Jitter (µs) | Max PER | Deadline Misses |
| --- | ---:| ---:| ---:| ---:|
| baseline_clean | 10 | 105.90 | 0.000 | 0 |
| moderate_drift_low_snr | 10 | 80.58 | 0.000 | 0 |
| burst_and_collision | 10 | 97.66 | 0.000 | 0 |
| severe_collision | 10 | 91.60 | 0.000 | 0 |

All scenarios completed without deadline overruns. Jitter peaks remain <110 µs, providing ≥2.5 ms slack at the most demanding SF. Collision-heavy profiles increase jitter variance but do not induce PER under the current seeds; future matrices should incorporate cumulative collision probability to gauge packet loss sensitivity.

`severe_collision` now emits a `tracking_failure` warning in the per-case summaries (header decode collapses when multiple collisions land back-to-back). Recommended mitigation: stretch the preamble or widen the coarse CFO/SFO search span before attempting payload decode.
