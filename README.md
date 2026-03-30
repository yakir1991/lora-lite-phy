# lora-lite-phy

A lightweight, standalone LoRa PHY-layer implementation in C++20.
Encodes and decodes LoRa packets entirely in software — no radio hardware or
GNU Radio required.

## Features

| Capability | Details |
|---|---|
| Spreading factors | SF5–SF12 (TX: SF6–SF12) |
| Bandwidths | 62.5 / 125 / 250 / 500 kHz |
| Coding rates | CR 4/5 – 4/8 |
| Header modes | Explicit and implicit |
| CRC | Optional (encode & verify) |
| Soft-decision decoding | Soft Hamming via FFT magnitude ratios |
| CFO tracking | Per-symbol EMA-based carrier frequency offset estimation |
| SFO compensation | Two-pass sweep with OS=2 upsample fallback |
| LDRO | Automatic low-data-rate optimisation |
| Sync words | Configurable (0x12 default, 0x34 LoRaWAN) |
| Multi-packet | Streaming decode of concatenated captures |
| Impairment injection | AWGN, CFO, SFO for controlled testing |

## Project layout

```
host_sim/           C++ core library, TX encoder, RX decoder
  src/              Source files (~7.5 k lines)
  include/          Public headers
  cmake/            CTest helper scripts
arduino/            RFM95 TX/RX sketches for OTA interop testing
tools/              Python & bash analysis/sweep scripts
docs/               Reverse-engineering notes, stage reports, hardware plans
third_party/        KissFFT (vendored), CMSIS-DSP
```

## Building

```bash
cmake -B build -G Ninja
cmake --build build
```

Requires CMake ≥ 3.16 and a C++20 compiler (GCC 11+ or Clang 14+).
No external dependencies beyond a C++ standard library.

## Running

### Encode a packet

```bash
./build/host_sim/lora_tx \
    --sf 7 --cr 1 --bw 125000 \
    --payload "Hello LoRa" \
    --output packet.cf32
```

Add impairments for robustness testing:

```bash
./build/host_sim/lora_tx \
    --sf 9 --bw 125000 --payload "Test" \
    --snr -10 --cfo 15000 --sfo 20 --seed 42 \
    --output impaired.cf32
```

### Decode a packet

```bash
./build/host_sim/lora_replay \
    --iq packet.cf32 \
    --metadata meta.json
```

The metadata JSON describes the capture parameters:

```json
{
  "sf": 7, "bw": 125000, "sample_rate": 500000,
  "cr": 1, "payload_len": 10, "has_crc": true,
  "implicit_header": false, "ldro": false,
  "preamble_len": 8, "sync_word": 18
}
```

Add `--soft` for soft-decision decoding, `--multi` for multi-packet
captures, `--verbose` for per-symbol debug output.

### Verify against a known payload

```bash
./build/host_sim/lora_replay \
    --iq packet.cf32 \
    --metadata meta.json \
    --payload "Hello LoRa"
```

Prints `CRC OK` / `CRC FAIL` and `Payload MATCH` / `Payload MISMATCH`.

## Tests

The project ships with **138 CTests** covering:

- OTA golden-file regression (real RFM95 captures, SF5–SF12)
- TX→RX roundtrip (SF6–SF12, CR 1–4, BW 62.5k–500k)
- Soft-decision decode under AWGN (SF6–SF12)
- Impairment sweep (CFO up to 40 kHz, SFO up to 50 ppm, combined)
- Implicit-header mode (SF7, SF10, SF12 with impairments)
- Multi-packet streaming, no-CRC, hex payload, LoRaWAN sync word

```bash
cd build && ctest -j$(nproc)
```

## TX CLI reference

| Flag | Default | Description |
|---|---|---|
| `--sf <6-12>` | 7 | Spreading factor |
| `--cr <1-4>` | 1 | Coding rate (4/5 … 4/8) |
| `--bw <Hz>` | 125000 | Bandwidth |
| `--sample-rate <Hz>` | bw | Output sample rate |
| `--preamble <n>` | 8 | Preamble symbols |
| `--sync-word <hex>` | 12 | Sync word (hex) |
| `--payload <text>` | — | ASCII payload |
| `--payload-hex <hex>` | — | Hex-encoded payload |
| `--output <file>` | — | Output .cf32 path |
| `--implicit` | off | Implicit header mode |
| `--no-crc` | off | Disable CRC |
| `--ldro` / `--no-ldro` | auto | Force LDRO on/off |
| `--snr <dB>` | off | Inject AWGN |
| `--cfo <Hz>` | 0 | Carrier frequency offset |
| `--sfo <ppm>` | 0 | Sampling frequency offset |
| `--seed <n>` | random | AWGN RNG seed |

## RX CLI reference

| Flag | Description |
|---|---|
| `--iq <file>` | Input .cf32 capture |
| `--metadata <file>` | Capture parameters JSON |
| `--payload <text>` | Expected payload for verification |
| `--summary <file>` | Write JSON decode summary |
| `--soft` | Enable soft-decision Hamming |
| `--multi` | Decode multiple packets |
| `--verbose` | Per-symbol debug output |

## Licence

See individual source files for licence information.
