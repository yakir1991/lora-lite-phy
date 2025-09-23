# LoRa Lite Standalone RX (no GNU Radio)

Minimal C++ receiver that detects a LoRa preamble and demodulates a few symbols using a simple state-machine approach. No GNU Radio dependency.

Status: prototype for SF7, integer-OS inputs (fs/bw = 1,2,4,8). Preamble detection via dechirp+naive DFT peak at bin 0.

## Build

This directory is a small CMake project.

- Configure + build:

```bash
cmake -S standalone -B standalone/build
cmake --build standalone/build -j
```

## Run

Input format: interleaved float32 IQ (I then Q) binary file.

```
standalone/build/lora_rx <iq_file> [sf=7] [bw=125000] [fs=250000]
```

Example:

```
standalone/build/lora_rx vectors/test_short_payload.unknown 7 125000 250000
```

## Notes

- Preamble detection: searches oversampling candidates and phases, then looks for min_syms consecutive bin-0 peaks.
- Demod: dechirp and naive DFT to pick the strongest integer bin per symbol.
- Limitations: no CFO/STO tracking, no header/payload FEC/whitening, fixed 16-symbol dump after preamble. Use this as a starting point.

## Next steps

- Add CFO and STO estimation from preamble (fractional and integer bins)
- Implement header detect/parse and payload length
- Deinterleaver, Hamming decoder, and dewhitening
- Switch naive DFT to FFT for speed
- Real-time streaming via ring buffer and full state machine transitions
