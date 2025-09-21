# LoRa GNU Radio Compatibility Pipeline

This repository contains a lightweight C++ implementation of the receive-side processing chain that mimics the behaviour of the GNU Radio `gr-lora_sdr` blocks. The code focuses solely on GNU Radio interoperability: it no longer ships the bespoke "LoRa Lite" PHY, transmit helpers, or the large collection of analysis scripts that previously existed in this repo.

## Directory Layout

- `include/lora/` – Public headers for the workspace container and GNU Radio compatible helpers.
- `src/` – Implementation of the FFT workspace, coarse synchronisation primitives, header decoding logic, and the end-to-end pipeline (`src/rx/gr_pipeline.cpp`).
- `test_gr_pipeline.cpp` – Small CLI utility that loads a complex IQ capture, runs the pipeline, and prints intermediate diagnostics.
- `python_bindings.cpp` – Python bindings for the C++ pipeline using pybind11.
- `build/` – Generated during CMake configuration (not tracked by Git).
- `vectors/` – Binary IQ captures used for quick experiments (e.g., `sps_125k_bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_nmsgs_8.unknown`).
- `scripts/` – Python scripts for running and comparing pipelines.

## Build Instructions

Ensure the vendored dependencies are available before configuring:

```bash
git submodule update --init external/liquid-dsp
```

The configure step will reuse an installed copy of `liquid-dsp` when one is
available on the host and otherwise builds the `external/liquid-dsp`
submodule so the pipeline can be compiled without additional manual setup.

```bash
cmake -S . -B build
cmake --build build
```

The default build produces three artefacts:

1. `liblora_gr.a` – the reusable static library that implements the pipeline.
2. `test_gr_pipeline` – a demo executable that operates on a single IQ vector.
3. `lora_pipeline.so` – Python module with bindings to the C++ pipeline.

The project depends on [liquid-dsp](https://github.com/jgaeddert/liquid-dsp) for FFT planning and [pybind11](https://github.com/pybind/pybind11) for Python bindings. CMake now falls back to the checked-in `external/liquid-dsp` sources if the system library is missing so future debugging sessions have a working FFT backend without additional provisioning.

## Running the Demo

### C++ Test Program

```bash
./build/test_gr_pipeline
```

The program reads the default IQ capture under `vectors/`, executes the GNU Radio–compatible pipeline, and prints frame synchronisation information, decoded header details, raw FEC nibbles, and payload bytes.

Pass `LORA_DEBUG=1` to see additional logs from the low-level primitives (e.g., preamble correlation, CFO/STO estimation steps):

```bash
LORA_DEBUG=1 ./build/test_gr_pipeline
```

### Python Scripts

The new C++ pipeline can also be used via Python scripts:

```bash
# Run the new pipeline on an IQ vector
python3 scripts/decode_offline_recording_final.py vectors/sps_125k_bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_nmsgs_8.unknown

# With custom parameters
python3 scripts/decode_offline_recording_final.py --sf 7 --bw 125000 --sync-word 0x34 vectors/your_vector.unknown
```

The Python script provides the same functionality as the C++ test program but with a more user-friendly interface and additional features like JSON output.

## Using the Workspace Utilities

`lora::Workspace` (defined in `include/lora/workspace.hpp`) encapsulates the reusable FFT buffers, upchirp/downchirp templates, and diagnostic scratch space used throughout the pipeline.  Each stage initialises the workspace with the desired spreading factor and then calls into the helpers provided under `include/lora/rx/gr/`:

- `primitives.hpp` – oversampling/decimation helpers, preamble detection, CFO/STO estimators, and hard-decision demodulation.
- `header_decode.hpp` – routines that reproduce GNU Radio’s explicit header decoding and Hamming(8,4) processing.
- `utils.hpp` – Gray mapping, diagonal deinterleaver map generation, and Hamming lookup tables.

## GNU Radio Utility Scripts

The `scripts/` directory contains both GNU Radio and new pipeline scripts:

### New Pipeline Scripts

- `decode_offline_recording_final.py` – Main script for running the new C++ pipeline on IQ vectors. Provides user-friendly output and supports various command-line options.

### GNU Radio Scripts

The following scripts wrap the canonical `gr-lora_sdr` blocks for comparison with the new pipeline. Activate the `gnuradio-lora` Conda environment before invoking them:

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

With the helper scripts above you can generate a canonical vector, inspect GNU Radio's intermediate buffers, and then run either `./build/test_gr_pipeline` or `python3 scripts/decode_offline_recording_final.py` on the same file. The expectation is that the decoded payload bytes and CRC outcomes will match between the two implementations.

## Debugging notes

- See [`docs/offline_decode_investigation.md`](docs/offline_decode_investigation.md) for the post-mortem on a failure where the
  offline decoder stopped after the header stage because the payload symbol count was hard-coded in the C++ pipeline. The notes
  summarise the symptom observed via `scripts/decode_offline_recording_final.py`, the fix in `src/rx/gr_pipeline.cpp`, and the
  open follow-ups that should be considered during future maintenance.

## New Pipeline Features

The new C++ pipeline (`src/rx/gr_pipeline.cpp`) includes several improvements over the original implementation:

- **Multi-frame decoding**: Automatically detects and decodes multiple LoRa frames in a single IQ stream
- **Improved CRC handling**: Correct endianness and algorithm matching GNU Radio's implementation
- **Better dewhitening**: Proper offset handling for each frame
- **Python bindings**: Direct integration with Python via pybind11
- **Enhanced diagnostics**: Detailed frame information and debug output

## Roadmap

- ✅ Re-enable automated parity checks against GNU Radio reference logs.
- ✅ Add regression tests that exercise multiple spreading factors and coding rates.
- ✅ Provide simple Python bindings around `liblora_gr.a` for quick scripting.
- Add support for additional LoRa parameters and configurations.
- Implement performance optimizations for real-time processing.

## Environment Setup

For GNU Radio scripts:
```bash
conda activate gnuradio-lora
export PYTHONPATH=$PWD/external/gr_lora_sdr/install/lib/python3.10/site-packages:$PYTHONPATH
export LD_LIBRARY_PATH=$PWD/external/gr_lora_sdr/install/lib:$LD_LIBRARY_PATH
```

For new pipeline scripts:
```bash
# Ensure pybind11 is installed
pip install pybind11

# Build the project
cmake -S . -B build
cmake --build build
```
