# GNU Radio vs. Standalone Receiver Comparison

This document captures the current head-to-head comparison between the GNU Radio reference (gr-lora_sdr) and the standalone host simulator receiver. It focuses on decode latency and qualitative robustness when CFO/STO/SFO impairments are injected to mimic on-air drift.

## Methodology
1. **Capture Set** – short reference recordings were selected for SF7/BW125/CR4/5 and SF9/BW125/CR4/8. Metadata lives under `docs/captures/session_sim_lab/`.
2. **Impairment Profiles** – three profiles are swept per capture:
   - `baseline`: clean link
   - `cfo_15ppm`: +15 ppm carrier offset
   - `combo_field`: −12 ppm CFO, +8 ppm SFO, −32 sample STO to mimic moving-node drift
3. **Tooling** – `tools/run_receiver_vs_gnuradio.py` applies impairments (via NumPy), runs GNU Radio decoding (`tools/gr_decode_capture.py`) inside the `gr310` conda env (with `PYTHONPATH`/`LD_LIBRARY_PATH` pointed at `gr_lora_sdr/install`), and runs `build/host_sim/lora_replay`. Runtime, resource usage, and the host-side summary JSON are captured.
4. **Outputs** – machine-readable results live in `docs/receiver_vs_gnuradio_results.json`, and the Markdown overview (suitable for reports) is `docs/receiver_vs_gnuradio_report.md`.
5. **Breadth** – the matrix now covers SF5/SF6/SF7/SF9/SF10/SF11/SF12 (varied CR/BW/LDRO). Longer captures can specify `max_samples` so the harness writes a trimmed CF32 on-the-fly before decoding.
6. **Resource Telemetry** – every decode invocation is wrapped with `/usr/bin/time -v`, so the JSON now carries `max_rss_kb` and `cpu_percent` for both GNU Radio and the standalone path (used by the RAM/CPU charts in the summary).

To reproduce:
```bash
conda run -n gr310 python -m pip install numpy  # once, if numpy missing
python tools/run_receiver_vs_gnuradio.py \
    docs/receiver_vs_gnuradio_matrix.json \
    --output-json docs/receiver_vs_gnuradio_results.json \
    --markdown docs/receiver_vs_gnuradio_report.md
```
Edit `docs/receiver_vs_gnuradio_matrix.json` to add further captures (e.g., the longer SF5/SF10 recordings) or new impairment profiles.

When runtime is a concern (especially for the long SF5/SF6/SF10 captures), you can run the harness in batches:

```bash
# Short references only
python tools/run_receiver_vs_gnuradio.py docs/receiver_vs_gnuradio_matrix.json \
    --captures sf7_bw125_cr1_short sf9_bw125_cr3_short \
    --output-json build/rvsg_part_short.json

# Long-form captures (each can be invoked separately)
python tools/run_receiver_vs_gnuradio.py docs/receiver_vs_gnuradio_matrix.json \
    --captures sf10_bw250_cr4_full \
    --output-json build/rvsg_sf10.json
```
The resulting JSON chunks can be merged (simple concatenation via a short Python helper) before publishing to `docs/receiver_vs_gnuradio_results.json`.

## Current Findings
See `docs/receiver_vs_gnuradio_report.md` for the full table. Highlights:

| Capture | Profile | GNU Radio Wall Time | Standalone Wall Time | Observations |
| --- | --- | --- | --- | --- |
| SF7/BW125/CR4/5 | Baseline | ~1.9 s | ~0.23 s | Standalone is ~8× faster while matching payloads/CRC. |
| SF7/BW125/CR4/5 | +15 ppm CFO | ~1.9 s | ~0.24 s | Both decoders stay locked; standalone summary shows negligible PER. |
| SF7/BW125/CR4/5 | Combo drift | ~2.0 s | ~0.23 s | Real-time impairments handled; GNU Radio remains slower but bit-aligned. |
| SF9/BW125/CR4/8 | Baseline | ~2.0 s | ~1.05 s | Longer symbol chains narrow the gap but standalone still ~2× faster. |
| SF9/BW125/CR4/8 | +15 ppm CFO | ~1.8 s | ~1.03 s | CFO stress shows no packet loss on either side; host summary records wider acquisition window. |
| SF9/BW125/CR4/8 | Combo drift | ~1.8 s | ~1.01 s | Even with concurrent CFO/SFO/STO offsets, both stacks decode successfully. |

Key takeaways:
- **Latency** – standalone decoding consistently undercuts the GNU Radio chain thanks to fixed-point optimisations and leaner buffering.
- **Real-Time Robustness** – the impairment engine (CFO/STO/SFO) demonstrates that the standalone loops remain stable under realistic drift; GNU Radio also recovers but with higher jitter.
- **Resource Usage** – `docs/receiver_vs_gnuradio_results.json` retains the underlying summaries (stage timing, PER, MTBF) so the data can be folded into CI dashboards.

## Next Steps
- Expand the capture list to cover the entire `docs/reference_stage_manifest.json` matrix once runtime permits (the tooling is agnostic—only the matrix file needs edits).
- Add AWGN/burst interference profiles so coexistence stress cases can be compared in the same framework.
- Feed the generated JSON into `tools/summarise_validation.py` (or a future dashboard) to visualize regression trends over time.
