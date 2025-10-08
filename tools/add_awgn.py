#!/usr/bin/env python3
# This file provides the 'add awgn' functionality for the LoRa Lite PHY toolkit.
"""
Add complex AWGN to a cf32 interleaved I/Q file at a target SNR (dB).

- Input: path to .cf32 (float32 little-endian, interleaved I Q), desired SNR in dB
- Output: writes a new .cf32 next to input with suffix _snr{SNR}dB and copies JSON with snr_db annotated

Usage:
  python tools/add_awgn.py input.cf32 --snr-db -15
  python tools/add_awgn.py input.cf32 --snr-db -10 --out-dir tmp_vectors
"""
# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations
# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports the module(s) numpy as np.
import numpy as np

# Defines the function add_awgn.
def add_awgn(in_path: Path, snr_db: float, out_dir: Path | None = None) -> Path:
    # Executes the statement `in_path = in_path.resolve()`.
    in_path = in_path.resolve()
    # Executes the statement `out_dir = (out_dir or in_path.parent).resolve()`.
    out_dir = (out_dir or in_path.parent).resolve()
    # Executes the statement `out_dir.mkdir(parents=True, exist_ok=True)`.
    out_dir.mkdir(parents=True, exist_ok=True)

    # Executes the statement `raw = np.fromfile(in_path, dtype=np.float32)`.
    raw = np.fromfile(in_path, dtype=np.float32)
    # Begins a conditional branch to check a condition.
    if raw.size % 2 != 0:
        # Raises an exception to signal an error.
        raise ValueError("Input cf32 has odd number of float32 values (I/Q mismatch)")
    # Executes the statement `I = raw[0::2].astype(np.float64)`.
    I = raw[0::2].astype(np.float64)
    # Executes the statement `Q = raw[1::2].astype(np.float64)`.
    Q = raw[1::2].astype(np.float64)
    # Executes the statement `x = I + 1j * Q`.
    x = I + 1j * Q

    # Executes the statement `sig_pow = float(np.mean(np.abs(x) ** 2))`.
    sig_pow = float(np.mean(np.abs(x) ** 2))
    # Begins a conditional branch to check a condition.
    if sig_pow <= 0.0:
        # Raises an exception to signal an error.
        raise ValueError("Signal power is non-positive")
    # Executes the statement `snr_lin = 10 ** (snr_db / 10.0)`.
    snr_lin = 10 ** (snr_db / 10.0)
    # Executes the statement `noise_pow = sig_pow / snr_lin  # Ps/Pn = SNR`.
    noise_pow = sig_pow / snr_lin  # Ps/Pn = SNR
    # Executes the statement `sigma = np.sqrt(noise_pow / 2.0)`.
    sigma = np.sqrt(noise_pow / 2.0)
    # Executes the statement `noise = (np.random.standard_normal(x.shape) + 1j * np.random.standard_normal(x.shape)) * sigma`.
    noise = (np.random.standard_normal(x.shape) + 1j * np.random.standard_normal(x.shape)) * sigma
    # Executes the statement `y = x + noise`.
    y = x + noise

    # Executes the statement `Io = np.asarray(np.real(y), dtype=np.float32)`.
    Io = np.asarray(np.real(y), dtype=np.float32)
    # Executes the statement `Qo = np.asarray(np.imag(y), dtype=np.float32)`.
    Qo = np.asarray(np.imag(y), dtype=np.float32)
    # Executes the statement `inter = np.empty(Io.size + Qo.size, dtype=np.float32)`.
    inter = np.empty(Io.size + Qo.size, dtype=np.float32)
    # Executes the statement `inter[0::2] = Io`.
    inter[0::2] = Io
    # Executes the statement `inter[1::2] = Qo`.
    inter[1::2] = Qo

    # Executes the statement `suffix = f"_snr{snr_db:+.0f}dB" if float(int(snr_db)) == snr_db else f"_snr{snr_db:+.1f}dB"`.
    suffix = f"_snr{snr_db:+.0f}dB" if float(int(snr_db)) == snr_db else f"_snr{snr_db:+.1f}dB"
    # Executes the statement `out_path = out_dir / f"{in_path.stem}{suffix}.cf32"`.
    out_path = out_dir / f"{in_path.stem}{suffix}.cf32"
    # Executes the statement `inter.tofile(out_path)`.
    inter.tofile(out_path)

    # Executes the statement `meta_src = in_path.with_suffix('.json')`.
    meta_src = in_path.with_suffix('.json')
    # Begins a conditional branch to check a condition.
    if meta_src.exists():
        # Executes the statement `meta = json.loads(meta_src.read_text())`.
        meta = json.loads(meta_src.read_text())
    # Provides the fallback branch when previous conditions fail.
    else:
        # Executes the statement `meta = {}`.
        meta = {}
    # Executes the statement `meta['snr_db'] = snr_db`.
    meta['snr_db'] = snr_db
    # Executes the statement `(out_path.with_suffix('.json')).write_text(json.dumps(meta, indent=2))`.
    (out_path.with_suffix('.json')).write_text(json.dumps(meta, indent=2))

    # Returns the computed value to the caller.
    return out_path


# Defines the function main.
def main() -> None:
    # Executes the statement `p = argparse.ArgumentParser(description="Inject AWGN into a cf32 vector at target SNR (dB)")`.
    p = argparse.ArgumentParser(description="Inject AWGN into a cf32 vector at target SNR (dB)")
    # Executes the statement `p.add_argument('input', type=Path, help='Input .cf32 path')`.
    p.add_argument('input', type=Path, help='Input .cf32 path')
    # Executes the statement `p.add_argument('--snr-db', type=float, required=True, help='Target SNR in dB (e.g., -15, -10, 0, 5)')`.
    p.add_argument('--snr-db', type=float, required=True, help='Target SNR in dB (e.g., -15, -10, 0, 5)')
    # Executes the statement `p.add_argument('--out-dir', type=Path, help='Optional output directory (default: same as input)')`.
    p.add_argument('--out-dir', type=Path, help='Optional output directory (default: same as input)')
    # Executes the statement `args = p.parse_args()`.
    args = p.parse_args()

    # Executes the statement `out = add_awgn(args.input, args.snr_db, args.out_dir)`.
    out = add_awgn(args.input, args.snr_db, args.out_dir)
    # Outputs diagnostic or user-facing text.
    print(str(out))

# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
