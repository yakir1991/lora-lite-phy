# OTA Interoperability Test Results

**Date:** 2026-03-03  
**Test:** First real over-the-air LoRa decode — ESP32+RFM95W → HackRF → Host Decoder

## Setup

| Component       | Detail |
|-----------------|--------|
| **Transmitter** | ESP32-D0WD-V3 + RFM95W 868MHz, spring antenna |
| **Receiver**    | HackRF One (serial a32868dc36620d47), telescopic antenna |
| **Distance**    | ~50 cm OTA (no attenuators, no cables) |
| **Frequency**   | 868.1 MHz |
| **LoRa Config** | SF7, BW 125kHz, CR 4/5, Sync 0x12, Preamble 8, CRC on |
| **TX Power**    | 10 dBm |
| **Payload**     | "LoRa Test #NNN" (14 bytes, explicit header) |

## Capture

- HackRF sample rate: 2 MHz, LNA gain 32 dB, IF gain 32 dB
- Capture length: 20 seconds (40M samples)
- **Packets detected:** 4 bursts at ~5 second intervals
- Burst duration: ~47–48 ms
- Peak power: +2.8 dB vs noise floor -33.4 dB (**SNR ≈ 36 dB**)

## Signal Processing Pipeline

1. **Burst detection** — Chunk-based power analysis to locate 4 LoRa packets
2. **CFO correction** — Preamble demodulated to bin 57/128 → 55.7 kHz offset corrected
   (combined HackRF + RFM95W crystal error)
3. **Burst trimming** — Extract 55 symbol windows around each packet
4. **Decimation** — 2 MHz → 250 kHz (8×) via scipy FIR + decimate

## Decode Results

### Host Decoder (lora-lite-phy `lora_replay`)

| Packet | Header | Payload | CRC |
|--------|--------|---------|-----|
| 0 | len=14, CR=1, CRC=1, CRC4✓ | `LoRa Test #520` | e5 33 |
| 1 | len=14, CR=1, CRC=1, CRC4✓ | `LoRa Test #521` | e4 33 |
| 2 | len=14, CR=1, CRC=1, CRC4✓ | `LoRa Test #522` | e7 33 |
| 3 | len=14, CR=1, CRC=1, CRC4✓ | `LoRa Test #523` | e6 33 |

### GNU Radio (gr-lora_sdr)

| Packet | Payload |
|--------|---------|
| 0 | `LoRa Test #520` |
| 1 | `LoRa Test #521` |
| 2 | `LoRa Test #522` |
| 3 | `LoRa Test #523` |

### Comparison

| Metric | Result |
|--------|--------|
| Packets decoded (host) | **4/4** (100%) |
| Packets decoded (GR) | **4/4** (100%) |
| **Host vs GR match** | **4/4** (100%) |
| Counter sequential | 520 → 523, zero loss |
| Header CRC valid | All 4 packets |

## Conclusion

**First successful OTA interop test.** The lora-lite-phy host decoder
correctly decodes real LoRa packets transmitted by commodity hardware
(Semtech SX1276 / RFM95W), matching GNU Radio gr-lora_sdr byte-for-byte
on all 4 captured packets.

### Notes

- The signal required CFO pre-correction (57 bins ≈ 56 kHz) before the host
  decoder's alignment function could lock onto the preamble. This is expected
  for a first OTA test — the alignment code was designed for simulation captures
  with zero or small CFO.
- GNU Radio's frame_sync handles large CFO internally and works on raw captures
  without pre-correction.
- Future improvement: add CFO-aware preamble search to `find_symbol_alignment()`.
