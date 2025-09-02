# LoRa Lite — Milestone 1 (Tables & Utilities)

This folder contains the initial utility modules for the MVP:
- `gray` (encode/decode + inverse table generator)
- `whitening` (configurable LFSR, pre-set PN9 helper)
- `hamming` (placeholders for CR 4/5..4/8 with a stable API)
- `interleaver` (placeholder diagonal mapping and dimensions)

Build & test:
```bash
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
cd build && ctest --output-on-failure
```
