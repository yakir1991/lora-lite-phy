# host_sim

Minimal C++ harness used to explore a standalone LoRa PHY implementation.

## Prerequisites

The GNU Radio reference blocks are vendored under `gr_lora_sdr/` and built via
the curated conda environment shipped with the upstream project:

```bash
source ~/miniconda3/etc/profile.d/conda.sh
conda activate gr310
export PYTHONPATH=$PWD/gr_lora_sdr/install/lib/python3.12/site-packages:$PYTHONPATH
export LD_LIBRARY_PATH=$PWD/gr_lora_sdr/install/lib:$LD_LIBRARY_PATH
```

## Build & Run

```bash
cmake -S . -B build
cmake --build build
# Replay a capture, emit capture stats and CI-friendly summary JSON
./build/lora_replay \
    --iq ../gr_lora_sdr/data/generated/tx_rx_sf7_bw125000_cr1_snrm5p0.cf32 \
    --compare-root ../gr_lora_sdr/data/generated/tx_rx_sf7_bw125000_cr1_snrm5p0.cf32 \
    --summary run_summary.json
```

## Test & Verification

All GNU Radio parity tests (plus manifest verification) are tagged with the
`host-sim` CTest label:

```bash
cmake --build build --target host-sim          # ctest -L host-sim
cmake --build build --target host-sim-regression  # with --output-on-failure
```

To spot-check a single capture:

```bash
./build/lora_replay \
    --iq ../gr_lora_sdr/data/generated/tx_rx_sf7_bw125000_cr1_snrm5p0_short.cf32 \
    --compare-root ../gr_lora_sdr/data/generated/tx_rx_sf7_bw125000_cr1_snrm5p0_short.cf32 \
    --summary /tmp/run.json
```

Integrity of the reference vectors can be validated independently:

```bash
python3 tools/check_reference_manifest.py docs/reference_stage_manifest.json \
    --data-dir gr_lora_sdr/data/generated
```

The harness now loads CF32 captures, aligns them against GNU Radio output, and
reports stage-by-stage parity, capture statistics, and payload verification
details for future integration work.
