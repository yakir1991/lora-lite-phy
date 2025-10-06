#!/usr/bin/env python3
"""
Add complex AWGN to a cf32 interleaved I/Q file at a target SNR (dB).

- Input: path to .cf32 (float32 little-endian, interleaved I Q), desired SNR in dB
- Output: writes a new .cf32 next to input with suffix _snr{SNR}dB and copies JSON with snr_db annotated

Usage:
  python tools/add_awgn.py input.cf32 --snr-db -15
  python tools/add_awgn.py input.cf32 --snr-db -10 --out-dir tmp_vectors
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import numpy as np

def add_awgn(in_path: Path, snr_db: float, out_dir: Path | None = None) -> Path:
    in_path = in_path.resolve()
    out_dir = (out_dir or in_path.parent).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    raw = np.fromfile(in_path, dtype=np.float32)
    if raw.size % 2 != 0:
        raise ValueError("Input cf32 has odd number of float32 values (I/Q mismatch)")
    I = raw[0::2].astype(np.float64)
    Q = raw[1::2].astype(np.float64)
    x = I + 1j * Q

    sig_pow = float(np.mean(np.abs(x) ** 2))
    if sig_pow <= 0.0:
        raise ValueError("Signal power is non-positive")
    snr_lin = 10 ** (snr_db / 10.0)
    noise_pow = sig_pow / snr_lin  # Ps/Pn = SNR
    sigma = np.sqrt(noise_pow / 2.0)
    noise = (np.random.standard_normal(x.shape) + 1j * np.random.standard_normal(x.shape)) * sigma
    y = x + noise

    Io = np.asarray(np.real(y), dtype=np.float32)
    Qo = np.asarray(np.imag(y), dtype=np.float32)
    inter = np.empty(Io.size + Qo.size, dtype=np.float32)
    inter[0::2] = Io
    inter[1::2] = Qo

    suffix = f"_snr{snr_db:+.0f}dB" if float(int(snr_db)) == snr_db else f"_snr{snr_db:+.1f}dB"
    out_path = out_dir / f"{in_path.stem}{suffix}.cf32"
    inter.tofile(out_path)

    meta_src = in_path.with_suffix('.json')
    if meta_src.exists():
        meta = json.loads(meta_src.read_text())
    else:
        meta = {}
    meta['snr_db'] = snr_db
    (out_path.with_suffix('.json')).write_text(json.dumps(meta, indent=2))

    return out_path


def main() -> None:
    p = argparse.ArgumentParser(description="Inject AWGN into a cf32 vector at target SNR (dB)")
    p.add_argument('input', type=Path, help='Input .cf32 path')
    p.add_argument('--snr-db', type=float, required=True, help='Target SNR in dB (e.g., -15, -10, 0, 5)')
    p.add_argument('--out-dir', type=Path, help='Optional output directory (default: same as input)')
    args = p.parse_args()

    out = add_awgn(args.input, args.snr_db, args.out_dir)
    print(str(out))

if __name__ == '__main__':
    main()
