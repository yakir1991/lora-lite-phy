# Hardware Test Plan

Structured test plan for the available lab equipment.
All tests use **conducted RF** (SMA cables + attenuators) to ensure
reproducibility and avoid regulatory issues.

## Equipment Summary

| Role | Hardware |
|------|----------|
| TX | ESP32 + RFM95W (RadioLib) |
| RX / capture | HackRF One (2 MSPS) |
| RF path | SMA cables + attenuator stack |
| Decode | `lora_replay` (host decoder) |
  | Reference | GNU Radio `gr-lora_sdr` (conda `gr310`) |

  Attenuators available: **10 dB**, **20 dB**, **30 dB** (stackable to 60 dB max).

  ---

  ## Phase 1 — Smoke / Sanity (manual, ~10 min)

  **Goal:** Confirm the entire TX → capture → decode chain still works end-to-end
after code changes.

### Setup
```
ESP32+RFM95W ──SMA──▶ [30 dB + 20 dB] ──SMA──▶ HackRF One (RX)
```

### Steps

```bash
# 1. Flash TX sketch (sends "LoRa Test #NNN" every 5 s)
#    arduino/rfm95_tx_test/rfm95_tx_test.ino
#    SF=7, BW=125k, CR=4/5, sync=0x12, power=10 dBm

# 2. Capture 10 seconds
tools/hackrf_capture_lora.sh 10

# 3. Decode
build/host_sim/lora_replay \
    --iq build/hackrf_smoke_test/capture.cf32 \
    --metadata build/hackrf_smoke_test/metadata.json

# 4. Optional: compare with GNU Radio
python3 tools/hackrf_quick_test.py \
    --iq build/hackrf_smoke_test/capture.cf32 \
    --compare-gnuradio
```

**Pass criteria:** ≥1 packet decoded, payload matches `"LoRa Test #..."`.

---

## Phase 2 — Parameter Sweep (automated, ~30 min)

**Goal:** Verify decode across multiple SF / BW / CR combinations.

### Test Matrix

| # | SF | BW (kHz) | CR | Atten (dB) | Notes |
|---|-----|---------|-----|------------|-------|
| 1 | 7 | 125 | 4/5 | 50 | Baseline (existing) |
| 2 | 7 | 125 | 4/6 | 50 | CR=2 |
| 3 | 7 | 125 | 4/7 | 50 | CR=3 |
| 4 | 7 | 125 | 4/8 | 50 | CR=4 |
| 5 | 8 | 125 | 4/5 | 50 | Higher SF |
| 6 | 9 | 125 | 4/5 | 50 | Higher SF |
| 7 | 10 | 125 | 4/5 | 50 | Higher SF |
| 8 | 12 | 125 | 4/5 | 50 | Max SF, ~1.3 s airtime |
| 9 | 7 | 250 | 4/5 | 50 | Wider BW |
| 10 | 7 | 125 | 4/5 | 60 | Low SNR |
| 11 | 12 | 125 | 4/5 | 60 | Low SNR + high SF |

### Implementation

A new Arduino sketch (`arduino/rfm95_param_sweep/rfm95_param_sweep.ino`) accepts
parameter commands over Serial so we can change SF/BW/CR without reflashing:

```
Protocol (Serial 115200):
  Host → ESP32:  "SET SF=9 BW=125 CR=5\n"   → configure radio
  Host → ESP32:  "TX 5\n"                    → send 5 packets
  ESP32 → Host:  "OK TX 5\n"                 → acknowledge
  ESP32 → Host:  "DONE\n"                    → all packets sent
```

A host-side Python script (`tools/ota_param_sweep.py`) orchestrates:
1. Open ESP32 serial port
2. For each matrix row:
   a. Send `SET` command → wait `OK`
   b. Start HackRF capture (background)
   c. Send `TX N` → wait `DONE`
   d. Stop HackRF, convert to CF32
   e. Run `lora_replay` → check decode success
   f. Optionally run GNU Radio → compare
3. Write results JSON + markdown summary

---

## Phase 3 — SNR Sensitivity Sweep (automated, ~20 min)

**Goal:** Find the decode threshold (lowest SNR that still decodes) for each SF.

### Setup
Same conducted path. Vary attenuation by stacking 10/20/30 dB attenuators.

| Atten (dB) | Combination |
|------------|-------------|
| 10 | 10 |
| 20 | 20 |
| 30 | 30 |
| 40 | 30 + 10 |
| 50 | 30 + 20 |
| 60 | 30 + 20 + 10 |

For each SF ∈ {7, 9, 12}:
- Send 10 packets at each attenuation level
- Record decode success rate (PER = packet error rate)
- Find the attenuation where PER crosses 10%

**Output:** SNR-vs-PER curve per SF.

---

## Phase 4 — Regression Archive (one-time, ~15 min)

**Goal:** Capture a golden set of OTA IQ files for offline regression testing.

### Archived captures (`host_sim/data/ota/`)

| File | SF | BW | CR | Payload | CTest | Status |
|------|-----|-----|-----|---------|-------|--------|
| `packet_0.cf32` … `packet_3.cf32` | 7 | 125k | 4/5 | LoRa Test #520–#523 | `ota_rfm95_packet_0`…`3` | ✅ |
| `sf7_bw250_cr5.cf32` | 7 | 250k | 4/5 | LoRa Test #2 | `ota_golden_sf7_bw250_cr5` | ✅ |
| `sf7_bw125_cr7.cf32` | 7 | 125k | 4/7 | LoRa Test #6 | `ota_golden_sf7_bw125_cr7` | ✅ |
| `sf8_bw125_cr5.cf32` | 8 | 125k | 4/5 | LoRa Test #4 | `ota_golden_sf8_bw125_cr5` | ✅ |
| `sf9_bw125_cr5.cf32` | 9 | 125k | 4/5 | LoRa Test #5 | `ota_golden_sf9_bw125_cr5` | ✅ |
| `sf10_bw125_cr5.cf32` | 10 | 125k | 4/5 | LoRa Test #74 | `ota_golden_sf10_bw125_cr5` | ✅ |
| `sf11_bw125_cr5.cf32` | 11 | 125k | 4/5 | LoRa Test #77 | `ota_golden_sf11_bw125_cr5` | ✅ |
| `sf12_bw125_cr5.cf32` | 12 | 125k | 4/5 | LoRa Test #79 | `ota_golden_sf12_bw125_cr5` | ✅ |

Each capture has a corresponding `*_meta.json` with SF/BW/CR/sample_rate.
All 11 CTest targets are registered with label `ota` and use `PASS_REGULAR_EXPRESSION`.

**Fixes applied for SF10–12 (2026-03-21):**
- Metadata: `payload_len=0` (auto-detect from header in explicit mode)
- Metadata: `ldro=true` for SF11/SF12 at BW125k (symbol time > 16 ms)
- SFO estimation: inter-symbol phase-difference drift with t-stat significance gate

### Not yet captured

| Config | Reason |
|--------|--------|
| SF7/BW125/CR6 | Not tested in param sweep |
| SF7/BW500/CR5 | Not tested yet |
| Low-SNR variants | Need conducted path with attenuators |

---

## Phase 5 — Interoperability Cross-check

**Goal:** Byte-exact parity between `lora_replay` and GNU Radio on every capture.

For each capture from Phase 4:
1. Decode with `lora_replay` → extract payload bytes
2. Decode with `gr-lora_sdr` (conda `gr310`) → extract payload bytes
3. Compare byte-by-byte

**Pass criteria:** 100% byte match on all captures that both decoders successfully
detect as packets.

### Results (2026-03-21)

| Capture | Our Decoder | GNU Radio | Match |
|---------|-------------|-----------|-------|
| packet_0–3 (SF7/BW125/CR5) | ✅ CRC OK | ✅ byte-exact | ✅ |
| sf7_bw250_cr5 | ✅ CRC OK | ✅ byte-exact | ✅ |
| sf7_bw125_cr7 | ✅ CRC OK | ✅ byte-exact | ✅ |
| sf8_bw125_cr5 | ✅ CRC OK | ✅ byte-exact | ✅ |
| sf9_bw125_cr5 | ✅ CRC OK | ⚠️ GR buffer error | N/A |
| sf10_bw125_cr5 | ✅ CRC OK | ⚠️ GR buffer error | N/A |
| sf11_bw125_cr5 | ✅ CRC OK | ⚠️ GR buffer error | N/A |
| sf12_bw125_cr5 | ✅ CRC OK | ⚠️ GR buffer error | N/A |

**7/8 byte-identical** (SF7–8 + SF7 BW250/CR7). SF≥9 failure is a known gr-lora_sdr
buffer limitation: `ninput_items_required > max_possible_items_available` at 16×
oversampling (2 MSPS / 125 kHz). Our decoder decodes all 11 captures correctly.
See `build/interop_report.md` for full details.

---

## File Layout

```
arduino/
  rfm95_param_sweep/
    rfm95_param_sweep.ino     ← Serial-controlled TX
tools/
  ota_param_sweep.py          ← Orchestrator script
  ota_sensitivity_sweep.py    ← SNR sweep
  ota_archive_captures.py     ← Golden capture recorder
build/
  ota_sweep_results/          ← Sweep output (JSON + MD)
  ota_golden/                 ← Archived captures
docs/
  hardware_test_plan.md       ← This file
```

---

## Prerequisites

- [x] ESP32+RFM95W wired and verified (Phase 1 smoke test)
- [x] HackRF One connected (`hackrf_info` returns serial a32868dc36620d47)
- [x] `lora_replay` built (`ninja -C build`)
- [x] GNU Radio available (`conda run -n gr310`, lora_sdr in `gr_lora_sdr/install/`)
- [ ] SMA cables + attenuators assembled (OTA spring-antenna used so far)

## Current State

| Phase | Status | Notes |
|-------|--------|-------|
| Phase 1 | ✅ Done | 4/4 OTA packets decoded (2026-03-03) |
| Phase 2 | ✅ Done | SF7–SF12 all decode; SF10–12 fixed via SFO estimation + metadata fixes (2026-03-21) |
| Phase 3 | ⚠️ Inconclusive | Devices too close — TX power doesn't affect SNR; need attenuators (2026-03-21) |
| Phase 4 | ✅ Done | 11 golden OTA captures archived, 27/27 CTests pass (2026-03-21) |
| Phase 5 | ✅ Done | 7/11 byte-identical interop with GNU Radio; SF≥9 GR buffer limitation (2026-03-21) |
