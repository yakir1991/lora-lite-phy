# LoRa GNU Radio Compatibility Pipeline

This repository contains a lightweight C++ implementation of the receive-side processing chain that mimics the behaviour of the GNU Radio `gr-lora_sdr` blocks.  The code focuses solely on GNU Radio interoperability: it no longer ships the bespoke “LoRa Lite” PHY, transmit helpers, or the large collection of analysis scripts that previously existed in this repo.

## Directory Layout

- `include/lora/` – Public headers for the workspace container and GNU Radio compatible helpers.
- `src/` – Implementation of the FFT workspace, coarse synchronisation primitives, header decoding logic, and the end-to-end pipeline (`src/rx/gr_pipeline.cpp`).
- `test_gr_pipeline.cpp` – Small CLI utility that loads a complex IQ capture, runs the pipeline, and prints intermediate diagnostics.
- `build/` – Generated during CMake configuration (not tracked by Git).
- `vectors/` – Binary IQ captures used for quick experiments (e.g., `sps_125k_bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_nmsgs_8.unknown`).

## Build Instructions

```bash
cmake -S . -B build
cmake --build build
```

The default build produces two artefacts:

1. `liblora_gr.a` – the reusable static library that implements the pipeline.
2. `test_gr_pipeline` – a demo executable that operates on a single IQ vector.

The project depends on [liquid-dsp](https://github.com/jgaeddert/liquid-dsp) for FFT planning.  The bundled CMake configuration expects the library to be available on the system (the Conda “gnuradio-lora” environment used in this workspace already provides it).

## Running the Demo

```bash
./build/test_gr_pipeline
```

The program reads the default IQ capture under `vectors/`, executes the GNU Radio–compatible pipeline, and prints frame synchronisation information, decoded header details, raw FEC nibbles, and payload bytes.

Pass `LORA_DEBUG=1` to see additional logs from the low-level primitives (e.g., preamble correlation, CFO/STO estimation steps):

```bash
LORA_DEBUG=1 ./build/test_gr_pipeline
```

## Using the Workspace Utilities

`lora::Workspace` (defined in `include/lora/workspace.hpp`) encapsulates the reusable FFT buffers, upchirp/downchirp templates, and diagnostic scratch space used throughout the pipeline.  Each stage initialises the workspace with the desired spreading factor and then calls into the helpers provided under `include/lora/rx/gr/`:

- `primitives.hpp` – oversampling/decimation helpers, preamble detection, CFO/STO estimators, and hard-decision demodulation.
- `header_decode.hpp` – routines that reproduce GNU Radio’s explicit header decoding and Hamming(8,4) processing.
- `utils.hpp` – Gray mapping, diagonal deinterleaver map generation, and Hamming lookup tables.

## GNU Radio Utility Scripts

The `scripts/` directory now contains lightweight helpers that wrap the canonical `gr-lora_sdr` blocks.  They are meant for quick spot checks against the C++ pipeline—no custom flowgraph editing required.  Activate the `gnuradio-lora` Conda environment before invoking them:

```bash
conda run -n gnuradio-lora <command>
```

- `generate_gnuradio_vector.py` – emits an interleaved `float32` IQ vector using the GNU Radio transmitter hierarchy.  Useful for creating fresh captures with a known payload.
  ```bash
  conda run -n gnuradio-lora python scripts/generate_gnuradio_vector.py \
      --payload text:"Hello LoRa" \
      --sf 7 --cr 1 --bw 125000 --samp-rate 250000 \
      --sync 0x34 --out vectors/gnuradio_sf7_cr45_ref.bin
  ```
- `analyze_gnuradio_vector.py` – runs the receive blocks on an IQ capture and dumps intermediate buffers (FFT bins, Gray symbols, header stream, etc.) so you can line up the behaviour with the C++ stages.
  ```bash
  conda run -n gnuradio-lora python scripts/analyze_gnuradio_vector.py \
      --iq vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_os2_sps250k.unknown \
      --sf 7 --cr 1 --bw 125000 --samp-rate 250000 --payload-len 65 --sync 0x34
  ```
- `run_gnuradio_payload.py` – feeds an IQ capture to the full GNU Radio receive chain and writes the decoded payload bytes.  Point it at the same vector you use with `test_gr_pipeline` to cross-check the payload contents.
  ```bash
  conda run -n gnuradio-lora python scripts/run_gnuradio_payload.py \
      --iq vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_os2_sps250k.unknown \
      --sf 7 --cr 1 --bw 125000 --samp-rate 250000 --payload-len 65 --sync 0x34
  ```

## Comparing with GNU Radio

With the helper scripts above you can generate a canonical vector, inspect GNU Radio’s intermediate buffers, and then run `./build/test_gr_pipeline` on the same file.  The expectation is that the decoded payload bytes and CRC outcomes will match between the two implementations.


## Roadmap

- Re-enable automated parity checks against GNU Radio reference logs.
- Add regression tests that exercise multiple spreading factors and coding rates.
- Provide simple Python bindings around `liblora_gr.a` for quick scripting.
