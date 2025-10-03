#!/usr/bin/env python3
"""
Symbol accuracy and confidence analysis for LoRa frames (FFT peak metrics).

Usage:
  python3 analysis/measure_symbol_accuracy.py \
      --vector vectors/hard/unknown_base_hard.cf32 \
      --sf 7 --pos 82000 --num 16

Outputs per-symbol:
  - detected bin (k_hat)
  - peak-to-second-peak ratio (dB)
  - peak magnitude and total energy
"""
import argparse
import struct
import numpy as np
from pathlib import Path


def load_cf32(path: Path) -> np.ndarray:
    data = path.read_bytes()
    floats = np.frombuffer(data, dtype='<f4')
    if len(floats) % 2 != 0:
        floats = floats[:-1]
    return floats[0::2] + 1j * floats[1::2]


def build_chirps(sf: int):
    N = 1 << sf
    n = np.arange(N, dtype=np.float64)
    up_phase = 2.0 * np.pi * (n * n / (2.0 * N) - n / 2.0)
    up = np.exp(1j * up_phase).astype(np.complex64)
    down = np.conj(up)
    return up, down


def symbol_metrics(samples: np.ndarray, sf: int, pos: int, count: int):
    N = 1 << sf
    _, down = build_chirps(sf)
    results = []
    for i in range(count):
        start = pos + i * N
        end = start + N
        if end > len(samples):
            break
        sym = samples[start:end]
        dechirped = sym * down
        fft = np.fft.fft(dechirped)
        mag = np.abs(fft)
        k_hat = int(np.argmax(mag))
        peak = float(mag[k_hat])
        # second peak
        mag2 = mag.copy()
        mag2[k_hat] = 0.0
        k2 = int(np.argmax(mag2))
        peak2 = float(mag2[k2]) if mag2.size else 0.0
        p2s = 20.0 * np.log10(max(peak, 1e-12) / max(peak2, 1e-12)) if peak2 > 0 else 99.0
        energy = float(np.sum(mag**2))
        results.append({
            'index': i,
            'k_hat': k_hat,
            'p2s_db': p2s,
            'peak': peak,
            'second_peak': peak2,
            'energy': energy,
        })
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--vector', required=True, type=Path)
    ap.add_argument('--sf', required=True, type=int)
    ap.add_argument('--pos', required=True, type=int, help='Start position (sample index)')
    ap.add_argument('--num', type=int, default=16, help='Number of symbols to analyze')
    args = ap.parse_args()

    samples = load_cf32(args.vector)
    N = 1 << args.sf
    if args.pos < 0 or args.pos + N > len(samples):
        raise SystemExit(f"Position out of range for file {args.vector}")

    print(f"Analyzing {args.num} symbols at pos {args.pos} (SF={args.sf}, N={N}) from {args.vector}")
    res = symbol_metrics(samples, args.sf, args.pos, args.num)
    good = 0
    for r in res:
        print(f"  sym {r['index']:2d}: k={r['k_hat']:3d}  P2S={r['p2s_db']:5.2f} dB  peak={r['peak']:.3e}  E={r['energy']:.3e}")
        if r['p2s_db'] >= 3.0:
            good += 1
    print(f"Summary: {len(res)} symbols analyzed; >=3 dB P2S: {good}/{len(res)} ({100.0*good/max(1,len(res)):.1f}%)")


if __name__ == '__main__':
    main()
