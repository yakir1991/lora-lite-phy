#!/usr/bin/env python3
"""Generate a set of explicit-header LoRa vectors with varying SF/BW/CR and AWGN down to -20 dB.

This composes the GNU Radio exporter (via tools/generate_random_vectors.py) and the AWGN injector
(tools/add_awgn.py) to produce a target count of vectors. Each output has a .cf32 and .json sidecar
including payload_hex and decode parameters so both GNU Radio and the C++ tools can consume them.

Defaults:
  - 100 vectors
  - SF in {7..12}, BW in {125k, 250k, 500k}, CR in {1..4}
  - explicit header, CRC on
  - SNR sampled uniformly from [-20, 10] dB (can be tightened via flags)

Usage:
  python -m tools.generate_noisy_explicit_set --out golden_vectors/new_batch --count 100
"""

from __future__ import annotations

import argparse
import json
import random
import subprocess
from pathlib import Path
from typing import List

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
GEN_RANDOM = ROOT / 'tools' / 'generate_random_vectors.py'
ADD_AWGN = ROOT / 'tools' / 'add_awgn.py'

SF_OPTIONS = [7, 8, 9, 10, 11, 12]
BW_OPTIONS = [125_000, 250_000, 500_000]
CR_OPTIONS = [1, 2, 3, 4]


def run(cmd: List[str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True)


def generate_base_vector(out_dir: Path, rng: np.random.Generator) -> Path:
    sf = int(rng.choice(SF_OPTIONS))
    cr = int(rng.choice(CR_OPTIONS))
    bw = int(rng.choice(BW_OPTIONS))
    fs = int(bw * int(rng.choice([2, 4, 8])))
    pay_len = int(rng.integers(8, 24))
    payload_hex = rng.integers(0, 256, pay_len, dtype=np.uint8).tobytes().hex()

    cmd = [
        'python', str(GEN_RANDOM),
        '--output', str(out_dir),
        '--sf', str(sf),
        '--cr', str(cr),
        '--bw', str(bw),
        '--sample-rate', str(fs),
        '--payload-hex', payload_hex,
        '--implicit-header',  # will toggle off by removing flag (we want explicit)
    ]
    # We want explicit header; do not pass --implicit-header
    cmd.remove('--implicit-header')

    res = run(cmd)
    if res.returncode != 0:
        raise RuntimeError(f"base gen failed: {res.stderr}\n{res.stdout}")

    # Find last tx_* file
    newest = max(out_dir.glob('tx_*.cf32'), key=lambda p: p.stat().st_mtime)
    # Ensure JSON has explicit header and CRC on
    meta_path = newest.with_suffix('.json')
    meta = json.loads(meta_path.read_text())
    meta.update({
        'impl_header': False,
        'implicit_header': False,
        'crc': True,
    })
    meta_path.write_text(json.dumps(meta, indent=2))
    return newest


def add_noise(base_cf32: Path, rng: np.random.Generator, out_dir: Path, snr_min: float, snr_max: float) -> Path:
    snr_db = float(rng.uniform(snr_min, snr_max))
    cmd = [
        'python', str(ADD_AWGN),
        str(base_cf32),
        '--snr-db', str(round(snr_db, 2)),
        '--out-dir', str(out_dir),
    ]
    res = run(cmd)
    if res.returncode != 0:
        raise RuntimeError(f"awgn failed: {res.stderr}\n{res.stdout}")
    path = Path(res.stdout.strip().splitlines()[-1])
    # Ensure explicit header flags remain explicit in output JSON
    meta_path = path.with_suffix('.json')
    meta = json.loads(meta_path.read_text())
    meta.update({'impl_header': False, 'implicit_header': False, 'crc': True})
    meta_path.write_text(json.dumps(meta, indent=2))
    return path


def main() -> None:
    ap = argparse.ArgumentParser(description='Generate explicit-header noisy LoRa vectors set')
    ap.add_argument('--out', type=Path, default=ROOT / 'golden_vectors' / 'new_batch', help='Output directory')
    ap.add_argument('--count', type=int, default=100)
    ap.add_argument('--seed', type=int, default=42)
    ap.add_argument('--snr-min', type=float, default=-20.0)
    ap.add_argument('--snr-max', type=float, default=10.0)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    base_dir = args.out / 'base_clean'
    noisy_dir = args.out
    base_dir.mkdir(parents=True, exist_ok=True)
    noisy_dir.mkdir(parents=True, exist_ok=True)

    paths: List[Path] = []
    for i in range(args.count):
        base = generate_base_vector(base_dir, rng)
        noisy = add_noise(base, rng, noisy_dir, args.snr_min, args.snr_max)
        paths.append(noisy)
        print(f"[gen] {i+1}/{args.count}: {noisy.name}")

    print(f"Generated {len(paths)} noisy explicit-header vectors in {noisy_dir}")


if __name__ == '__main__':
    main()
