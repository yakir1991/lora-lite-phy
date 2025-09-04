# LoRa Lite PHY — Utilities & Loopback

This repository hosts the first pieces of a stand‑alone LoRa® physical layer.  
Current focus is on reusable utility modules and a deterministic loopback
transmitter/receiver pair used for bring‑up and testing.

## Utility modules
- `gray` – encode/decode helpers and inverse table generation
- `whitening` – configurable LFSR with a PN9 helper
- `hamming` – CR 4/5 through 4/8 encode/decode tables
- `interleaver` – diagonal mapping and size helpers

## Loopback TX/RX
`lora/tx/loopback_tx.hpp` converts a payload into IQ samples, while
`lora/rx/loopback_rx.hpp` demodulates those samples back to bytes and checks the
CRC.  The end‑to‑end loopback exercise is demonstrated in
`tests/test_loopback.cpp`.

## Build
```bash
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## Dependencies
- `cmake` (>= 3.16), `ninja`, C++20 compiler
- `libliquid-dev` (Liquid-DSP FFT backend)
- `python3` (only for exporting reference vectors)

Optional (to generate GNU Radio reference vectors):
- `conda` or `mamba` with `gnuradio` 3.10 and `gnuradio-lora_sdr` available

On Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build libliquid-dev python3
```

## Run tests
```bash
cd build
ctest --output-on-failure             # run all tests
ctest -R Loopback --output-on-failure # run only loopback test
```

## Reference vectors (optional)
Reference vectors are generated from the GNU Radio LoRa SDR blocks when available.
Recommended, isolated setup using conda:
```bash
conda create -n gnuradio-lora -c conda-forge gnuradio=3.10 cmake ninja -y
conda activate gnuradio-lora
```
Then generate vectors:
```bash
cmake --build build --target export_vectors
```
The vectors are stored under `vectors/` and copied into the build tree automatically.
If GNU Radio is not available, the export target will fail (by design) to ensure
vectors remain aligned with the true reference implementation.

## Remaining MVP work
- Integrate a single FFT backend and precomputed chirps
- Complete TX/RX pipeline over preallocated buffers
- Cross-validate against `gr-lora_sdr` vectors and capture performance metrics
- Add basic synchronization (preamble detection and timing/Fo offset)

See [LoRa_Lite_Migration_Plan_README.md](LoRa_Lite_Migration_Plan_README.md)
for detailed roadmap and context.
