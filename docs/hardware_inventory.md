# Hardware Inventory

Lab equipment available for LoRa PHY development and testing.

## SDR

| Item | Qty | Details |
|------|-----|---------|
| HackRF One | 1 | Serial `a32868dc36620d47`, telescopic antenna |

## LoRa Modules

| Item | Details |
|------|---------|
| ESP32-D0WD-V3 | Micro-controller, wired to RFM95W via SPI |
| RFM95W 868 MHz | LoRa transceiver (Semtech SX1276), spring antenna |

## RF Accessories

| Item | Qty |
|------|-----|
| Attenuator 10 dB | 1 |
| Attenuator 20 dB | 1 |
| Attenuator 30 dB | 1 |
| SMA male ↔ SMA male cable | 2 |
| Dummy load 50 Ω | 2 |

## Debug Tools

| Item | Qty |
|------|-----|
| Logic Analyzer | 1 |

## Verified OTA Configuration

The following setup was used to capture the four reference OTA packets
(`packet_0.cf32` – `packet_3.cf32`).

| Role | Hardware | Settings |
|------|----------|----------|
| TX | ESP32 + RFM95W | 10 dBm, spring antenna, 868.0 MHz, SF7, BW 125 kHz, CR 4/5, sync 0x12 |
| RX | HackRF One | 2 MHz sample rate, LNA 32 dB, IF 32 dB, 868.1 MHz, telescopic antenna |

- **Distance:** ~50 cm OTA (no attenuators, no cables)
- **Result:** 4/4 packets captured and decoded successfully by both GNU Radio
  gr-lora_sdr and the project's native decoder (`lora_replay`).

### Decoded Payloads

```
packet_0: LoRa Test #520
packet_1: LoRa Test #521
packet_2: LoRa Test #522
packet_3: LoRa Test #523
```
