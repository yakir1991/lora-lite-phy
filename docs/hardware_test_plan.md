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

### Captures to record

| File | SF | BW | CR | Atten | Packets |
|------|-----|-----|-----|-------|---------|
| `ota_sf7_bw125_cr1.cf32` | 7 | 125k | 4/5 | 50 dB | 5 |
| `ota_sf7_bw125_cr2.cf32` | 7 | 125k | 4/6 | 50 dB | 5 |
| `ota_sf9_bw125_cr1.cf32` | 9 | 125k | 4/5 | 50 dB | 3 |
| `ota_sf12_bw125_cr1.cf32` | 12 | 125k | 4/5 | 50 dB | 2 |
| `ota_sf7_bw250_cr1.cf32` | 7 | 250k | 4/5 | 50 dB | 5 |
| `ota_sf7_bw125_cr1_lowsnr.cf32` | 7 | 125k | 4/5 | 60 dB | 5 |

These captures become CTest fixtures (offline decode, no hardware needed):
```cmake
add_test(NAME ota_sf7_bw125_cr1_decode
    COMMAND lora_replay --iq ${OTA_DATA}/ota_sf7_bw125_cr1.cf32 ...)
```

---

## Phase 5 — Interoperability Cross-check

**Goal:** Byte-exact parity between `lora_replay` and GNU Radio on every capture.

For each capture from Phase 4:
1. Decode with `lora_replay` → extract payload bytes
2. Decode with `gr-lora_sdr` (conda `gr310`) → extract payload bytes
3. Compare byte-by-byte

**Pass criteria:** 100% byte match on all captures that both decoders successfully
detect as packets.

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

- [ ] ESP32+RFM95W wired and verified (Phase 1 smoke test)
- [ ] HackRF One connected (`hackrf_info` returns serial)
- [ ] `lora_replay` built (`ninja -C build`)
- [ ] GNU Radio available (`conda run -n gr310 python -c "from gnuradio import lora_sdr"`)
- [ ] SMA cables + attenuators assembled

## Current State

| Phase | Status | Notes |
|-------|--------|-------|
| Phase 1 | ✅ Done | 4/4 OTA packets decoded (2026-03-03) |
| Phase 2 | ⬜ Not started | Need param-sweep Arduino sketch + script |
| Phase 3 | ⬜ Not started | Needs Phase 2 infrastructure |
| Phase 4 | ⬜ Not started | Needs Phase 2 captures |
| Phase 5 | ⬜ Not started | Needs Phase 4 captures |
