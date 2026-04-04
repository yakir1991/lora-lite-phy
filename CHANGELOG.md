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
- `--cfo-track [alpha]` CLI flag for per-symbol CFO tracking (replaces env var)
- `--per-stats` CLI flag for PER/BER statistics
- `--multi-sf` CLI flag for gateway-style multi-SF decode
- OS=2 upsample fallback in streaming decode path
- Byte-exact payload verification (`--payload`) with `payload_failure` vs `compare_failure`
- CRC mismatch sets `payload_failure` when `--payload` is given
- Link-time optimisation (LTO) via `CMAKE_INTERPROCEDURAL_OPTIMIZATION`
- Doxygen target wired to CMake (`cmake --build build --target docs`)
- Code coverage CMake option (`-DENABLE_COVERAGE=ON`, `cmake --build build --target coverage`)
- CMake `project(VERSION 0.2.0)` for programmatic version queries
- 2 streaming OS=2 fallback tests (SF7, SF9)
- 2 soft-decode tests (SF10, SF11)
- 11 new impairment tests (triple CFO+SFO+AWGN, combined at higher SFs/CRs)

### Fixed
- CI timeout: exclude `tx_soft_sf12` (>300s) and set `--timeout 300`
- Stale test counts in CONTRIBUTING.md and CHANGELOG.md
- `.gitignore` now covers `coverage_html/`
- `host_sim_summary_metrics` marked `RUN_SERIAL` to fix parallel flakiness
- Doxygen warning-free (missing `@param` tags on `detect_burst_ex`)

### CI
- `clang-format-17` check on every PR
- Upload `LastTest.log` artifact on test failure
- Code coverage via lcov + Codecov upload on master push
- Release workflow: `git tag v*` → GitHub Release with Linux binaries

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
- 137 CTests: GNU Radio parity, OTA golden-file, TX roundtrip, soft-decision, impairment sweep
- CI pipeline (GCC-13, Clang-17) on GitHub Actions
- Arduino RFM95 sketches for OTA interoperability testing
- Reverse-engineering documentation with 12 figures
- 40+ golden OTA captures from RFM95 hardware
