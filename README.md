# lora-lite-phy

A lightweight, standalone LoRa PHY-layer implementation in C++20.
Encodes and decodes LoRa packets entirely in software — no radio hardware or
GNU Radio required.

Verified bit-exact against the
[gr-lora_sdr](https://github.com/tapparelj/gr-lora_sdr) GNU Radio reference
across all SF/BW/CR combinations, and validated over-the-air with a Semtech
RFM95 transceiver captured via HackRF One SDR.

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
  "sf": 7,
  "bw": 125000,
  "sample_rate": 500000,
  "cr": 1,
  "payload_len": 10,
  "has_crc": true,
  "implicit_header": false,
  "ldro": false,
  "preamble_len": 8,
  "sync_word": 18
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `sf` | int | yes | Spreading factor (5–12) |
| `bw` | int | yes | Bandwidth in Hz |
| `sample_rate` | int | yes | Capture sample rate in Hz |
| `cr` | int | yes | Coding rate (1–4, meaning 4/5–4/8) |
| `payload_len` | int | no | Expected payload length in bytes (0 = from header) |
| `has_crc` | bool | no | Whether CRC is present (default: true) |
| `implicit_header` | bool | no | Implicit header mode (default: false) |
| `ldro` | bool | no | Low data-rate optimisation (default: auto) |
| `preamble_len` | int | no | Preamble length in symbols (default: 8) |
| `sync_word` | int | no | Sync word value (default: 0x12; LoRaWAN: 0x34) |

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

The project ships with **152 CTests** (when GNU Radio reference data is
present locally; CI runs the ~136 self-contained tests):

| Category | Count | Description |
|---|---|---|
| GNU Radio parity | 11 | Bit-exact match against `gr_lora_sdr` reference output |
| Unit / integration | 6 | Numeric traits, Q15 demod, scheduler, summary metrics |
| OTA golden-file | 44 | Real RFM95 captures, SF6–SF12, BW 125–500 kHz, CR 4/5–4/8 |
| TX→RX roundtrip | 15 | SF6–SF12, CR 1–4, BW 62.5k–500k, OS=1/OS=4 |
| Soft-decision AWGN | 8 | SF6–SF12 under noise, soft Hamming decode |
| Impairment sweep | 51 | CFO ≤40 kHz, SFO ≤50 ppm, combined triple (CFO+SFO+AWGN) |
| Streaming OS=2 | 2 | High-SFO streaming decode with OS=2 upsample fallback |
| Implicit header | 11 | SF7, SF10, SF12 with impairments |
| Misc | 4 | Multi-packet, no-CRC, hex payload, LoRaWAN sync word |

```bash
cd build && ctest -j$(nproc)
```

### API Documentation (Doxygen)

```bash
cmake --build build --target docs
# Output: docs/api/html/index.html
```

### Code Coverage

```bash
cmake -B build -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest -j$(nproc)
cmake --build . --target coverage
# Output: build/coverage_html/index.html
```

## Documentation

The [reverse-engineering paper](docs/rev_eng_lora.md) provides a detailed
technical walkthrough of the LoRa PHY layer — modulation, coding, packet
structure, synchronisation, and demodulation — including 12 figures and
measured FER results against hardware.

| Report | Description |
|--------|-------------|
| [GNU Radio Compatibility](docs/gnu_radio_compatibility_report.md) | Payload-matching results across 72 SF/BW/CR configurations and 59 impairment scenarios |
| [Performance Sweep](docs/receiver_vs_gnuradio_sweep_report.md) | Decode latency comparison — standalone vs gr-lora_sdr |

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
| `--iq <file>` | Input .cf32 capture (use `-` for stdin) |
| `--format cf32\|hackrf` | IQ sample format (default: cf32) |
| `--metadata <file>` | Capture parameters JSON |
| `--payload <text>` | Expected payload for byte-exact verification |
| `--summary <file>` | Write JSON decode summary |
| `--soft` | Enable soft-decision Hamming |
| `--multi` | Decode multiple packets in one capture |
| `--multi-sf` | Probe SF6–SF12 per burst (auto-detect spreading factor) |
| `--stream` | Streaming mode: decode packets as they arrive (implies `--iq - --multi`) |
| `--per-stats` | Print PER/BER statistics at end of streaming run |
| `--cfo-track [alpha]` | Enable per-symbol CFO tracking EMA (default α=0.02) |
| `--verbose` | Per-symbol debug output |

## Live OTA Decode (HackRF One)

The decoder has been validated end-to-end over the air using:

- **TX**: Semtech RFM95W on ESP32 (RadioLib), SF12 BW125 CR4/8 at 868.1 MHz
- **RX**: HackRF One SDR at 2 MHz sample rate

Three consecutive packets were decoded successfully with CRC-verified payloads:

```
Payload ASCII: LoRa Test #135154   CRC 0x62c0 OK
Payload ASCII: LoRa Test #135155   CRC 0x62c1 OK
Payload ASCII: LoRa Test #135156   CRC 0x62c2 OK
```

To capture and decode live packets:

```bash
# Capture at 200 kHz offset to avoid HackRF DC spike
hackrf_transfer -r /dev/stdout -f 867900000 -s 2000000 \
    -l 24 -g 24 -n 40000000 | \
  ./build/host_sim/lora_replay \
    --iq - --format hackrf \
    --metadata ota_meta.json \
    --multi --verbose
```

With metadata:

```json
{
  "sample_rate": 2000000,
  "bw": 125000,
  "sf": 12,
  "cr": 0,
  "preamble_len": 8,
  "has_crc": true,
  "ldro": true,
  "implicit_header": false,
  "sync_word": 18
}
```

> **Note:** Set `cr` to `0` to accept whatever coding rate the header reports.
> The 200 kHz frequency offset avoids the HackRF DC spur; the decoder's
> CFO tracking compensates automatically.

## Acknowledgments

- **[gr-lora_sdr](https://github.com/tapparelj/gr-lora_sdr)** by Joachim Tapparel
  et al. (EPFL TCL) — GNU Radio LoRa transceiver used as the bit-exact
  reference for payload verification.  Licensed under GPL-3.0.
- **[KissFFT](https://github.com/mborgerding/kissfft)** by Mark Borgerding —
  vendored FFT library (float + Q15 builds).  Licensed under BSD-3-Clause.
- **[RadioLib](https://github.com/jgromes/RadioLib)** by Jan Gromeš —
  Arduino library used in the RFM95 OTA test sketches.  Licensed under MIT.

## Licence

Non-commercial — free for personal, academic, and research use.
Commercial use requires prior written permission from the author.
See [LICENSE](LICENSE) for full terms.
