# lora-lite-phy

A lightweight, standalone LoRa PHY-layer implementation in C++20.
Encodes and decodes LoRa packets entirely in software — no radio hardware or
GNU Radio required.

Verified bit-exact against the
[gr-lora_sdr](https://github.com/tapparelj/gr-lora_sdr) GNU Radio reference
across all SF/BW/CR combinations, and validated over-the-air with a Semtech
RFM95 transceiver.

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
| Fixed-point demod | Native Q15 FFT pipeline (KissFFT FIXED_POINT=16) |
| LDRO | Automatic low-data-rate optimisation |
| Sync words | Configurable (0x12 default, 0x34 LoRaWAN) |
| Multi-packet | Streaming decode of concatenated captures |
| Impairment injection | AWGN, CFO, SFO for controlled testing |

## Architecture

```
host_sim/
  include/host_sim/   Public headers (demod, chirp, hamming, scheduler, …)
  src/                 Core library + TX encoder + RX decoder (~7.5 k LoC)
  tests/               Unit & integration tests (Q15, scheduler, stage-processing)
  cmake/               CTest helper scripts
  data/ota/            Golden OTA captures from an RFM95 transceiver
  third_party/kissfft/ Vendored KissFFT (float + Q15 builds)
arduino/               RFM95 TX/RX/param-sweep sketches for OTA interop
docs/                  Reverse-engineering paper + technical figures
tools/                 CI helper scripts (manifest checker, metrics comparator)
```

**Signal-processing pipeline:**

1. **Burst detection** — energy + preamble chirp correlation
2. **Time/frequency alignment** — sub-sample STO refinement, coarse CFO from SFD
3. **Dechirp + FFT demodulation** — float or native Q15 path, parabolic peak interpolation
4. **Per-symbol CFO tracking** — EMA filter on fractional-bin residuals
5. **SFO compensation** — two-pass grid sweep; OS=2 upsample fallback
6. **Gray decode → deinterleave → Hamming FEC → de-whitening → CRC**

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

The project ships with **138 CTests** (when GNU Radio reference data is
present locally; CI runs the ~123 self-contained tests):

| Category | Count | Description |
|---|---|---|
| GNU Radio parity | 15 | Bit-exact match against `gr_lora_sdr` reference output |
| OTA golden-file | 44 | Real RFM95 captures, SF6–SF12, BW 125–500 kHz, CR 4/5–4/8 |
| TX→RX roundtrip | 9 | SF6–SF12, CR 1–4, BW 62.5k–500k |
| Soft-decision AWGN | 6 | SF6–SF12 under noise, soft Hamming decode |
| Impairment sweep | 52 | CFO ≤40 kHz, SFO ≤50 ppm, combined |
| Implicit header | 11 | SF7, SF10, SF12 with impairments |
| Misc | 1 | Multi-packet, no-CRC, hex payload, LoRaWAN sync word |

```bash
cd build && ctest -j$(nproc)
```

## Documentation

The [reverse-engineering paper](docs/rev_eng_lora.md) provides a detailed
technical walkthrough of the LoRa PHY layer — modulation, coding, packet
structure, synchronisation, and demodulation — including 12 figures and
measured FER results against hardware.

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

MIT — see individual source files for details.
