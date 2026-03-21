# Simulated Lab Session (SF7 / BW125)

- Source: `gr_lora_sdr/apps/simulation/flowgraph/tx_rx_simulation.py`
- Scenario: single-link reference at +5 dB SNR, coding rate 4/5.
- Purpose: baseline IQ to drive `host_sim_live_soak` capture-mode regression until OTA recordings exist.

Artifacts:
- IQ (SF7/CR4/5): `../../gr_lora_sdr/data/generated/tx_rx_sf7_bw125000_cr1_snrm5p0_short.cf32`
- Metadata: `iq/tx_rx_sf7_bw125000_cr1_snrm5p0_short.json`
- IQ (SF9/CR4/8): `../../gr_lora_sdr/data/generated/tx_rx_sf9_bw125000_cr3_snrm2p0_short.cf32`
- Metadata: `iq/tx_rx_sf9_bw125000_cr3_snrm2p0_short.json`
- Manifest: `capture_manifest.json`
