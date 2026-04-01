# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Real-time pipe support: `--iq -` reads IQ samples from stdin
- `--format hackrf` for direct HackRF INT8 decoding without conversion
- `tools/hackrf_live_decode.sh` helper for one-command live capture + decode
- `CONTRIBUTING.md` with development workflow and conventions
- `.clang-format` for consistent code style
- `CHANGELOG.md` (this file)
- CMake install targets (`cmake --install build`)
- Doxyfile for API documentation generation
- `examples/` directory with minimal TX/RX usage examples

## [0.1.0] — 2026-03-15

### Added
- Full LoRa PHY-layer encoder (`lora_tx`) and decoder (`lora_replay`)
- Spreading factors SF5–SF12 (TX: SF6–SF12)
- Bandwidths 62.5 / 125 / 250 / 500 kHz, coding rates CR 4/5–4/8
- Explicit and implicit header modes
- Optional CRC encode and verify
- Soft-decision Hamming decoding via FFT magnitude ratios
- Per-symbol EMA-based CFO tracking
- Two-pass SFO compensation with OS=2 upsample fallback
- Native Q15 fixed-point FFT pipeline (KissFFT)
- LDRO (low data-rate optimisation) automatic detection
- Configurable sync words (0x12 default, 0x34 LoRaWAN)
- Multi-packet streaming decode (`--multi`)
- Impairment injection (AWGN, CFO, SFO) for controlled testing
- 138 CTests: GNU Radio parity, OTA golden-file, TX roundtrip, soft-decision, impairment sweep
- CI pipeline (GCC-13, Clang-17) on GitHub Actions
- Arduino RFM95 sketches for OTA interoperability testing
- Reverse-engineering documentation with 12 figures
- 40+ golden OTA captures from RFM95 hardware
