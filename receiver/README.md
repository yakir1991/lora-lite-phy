# Python LoRa Receiver (modular)

This package contains the modularized Python LoRa receiver used for GNU Radio compatibility exploration.

Structure:
- io.py — CF32 loading utilities
- utils.py — shared math/Gray/utilities
- demod.py — symbol demodulation (FFT-based, oversample folding)
- sync.py — sync word helpers and header search/scan
- decode.py — payload symbol mapping -> bytes (adapts to lora_decode_utils)
- receiver.py — high-level orchestrator class `LoRaReceiver`
- cli.py — command-line interface (kept compatible with `complete_lora_receiver.py`)

Usage examples:
- python -m receiver.cli <file.cf32> --sf 7 --bw 125000 --cr 2 --ldro-mode 2 --samp-rate 500000 -v
- python complete_lora_receiver.py <file.cf32> --sf 7 --bw 125000 --cr 2 --ldro-mode 2 --samp-rate 500000 -v

Notes:
- The top-level script `complete_lora_receiver.py` remains as a thin wrapper for backward compatibility.
- For 100% parity with GNU Radio, use the scripts and test suites under `scripts/` and `tests/test_gnu_radio_compat.py` as documented in `docs/GNU_RADIO_COMPAT.md`.
