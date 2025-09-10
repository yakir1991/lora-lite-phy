# Notes on gr-lora_sdr Integration

- This repository uses `gnuradio.lora_sdr` only for generating reference vectors.
- To make batch generation deterministic and non-blocking, Throttle blocks are removed from the flowgraphs used by the helper scripts.
  - See `scripts/export_vectors_grc.sh` and `scripts/strip_throttle_blocks.py` (they remove any `blocks_throttle` instances from `.grc` files before running).
  - Some example Python flowgraphs in `external/gr_lora_sdr` were also adjusted to comment out Throttle connections in headless runs (e.g., `apps/simulation/flowgraph/tx_rx_simulation.py`).
- Removing Throttle does not change the DSP or frame contents; it only prevents flow control from slowing down or stalling batch jobs.

