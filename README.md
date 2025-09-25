# lora-lite-phy (standalone rewrite in progress)

Repository cleaned on 2025-09-25. Only `external/`, `docs/`, and `vectors/` retained.
Goal: Rebuild a standalone LoRa PHY mirroring gr-lora_sdr functionality without GNU Radio runtime.

Next steps (planned):
1. Extract core DSP + FEC primitives from `external/gr_lora_sdr` into new `src/` tree (pure C++).
2. Remove GNU Radio block inheritance; adapt processing into plain functions operating on buffers.
3. Provide CMake build producing a static + shared library and simple CLI tools (tx, rx, loopback, vector gen).
4. Add unit tests (CRC, whitening, Hamming, interleaver, modem, header, payload, end-to-end loopback).
5. Validate against vectors in `vectors/`.

Backup reference branch/tag: backup-pre-clean-20250925
