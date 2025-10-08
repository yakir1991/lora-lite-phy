#!/usr/bin/env python3
# This file provides the 'generate noisy explicit set' functionality for the LoRa Lite PHY toolkit.
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

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports the module(s) random.
import random
# Imports the module(s) subprocess.
import subprocess
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from typing import List'.
from typing import List

# Imports the module(s) numpy as np.
import numpy as np

# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `GEN_RANDOM = ROOT / 'tools' / 'generate_random_vectors.py'`.
GEN_RANDOM = ROOT / 'tools' / 'generate_random_vectors.py'
# Executes the statement `ADD_AWGN = ROOT / 'tools' / 'add_awgn.py'`.
ADD_AWGN = ROOT / 'tools' / 'add_awgn.py'

# Executes the statement `SF_OPTIONS = [7, 8, 9, 10, 11, 12]`.
SF_OPTIONS = [7, 8, 9, 10, 11, 12]
# Executes the statement `BW_OPTIONS = [125_000, 250_000, 500_000]`.
BW_OPTIONS = [125_000, 250_000, 500_000]
# Executes the statement `CR_OPTIONS = [1, 2, 3, 4]`.
CR_OPTIONS = [1, 2, 3, 4]


# Defines the function run.
def run(cmd: List[str]) -> subprocess.CompletedProcess:
    # Returns the computed value to the caller.
    return subprocess.run(cmd, capture_output=True, text=True)


# Defines the function generate_base_vector.
def generate_base_vector(out_dir: Path, rng: np.random.Generator) -> Path:
    # Executes the statement `sf = int(rng.choice(SF_OPTIONS))`.
    sf = int(rng.choice(SF_OPTIONS))
    # Executes the statement `cr = int(rng.choice(CR_OPTIONS))`.
    cr = int(rng.choice(CR_OPTIONS))
    # Executes the statement `bw = int(rng.choice(BW_OPTIONS))`.
    bw = int(rng.choice(BW_OPTIONS))
    # Executes the statement `fs = int(bw * int(rng.choice([2, 4, 8])))`.
    fs = int(bw * int(rng.choice([2, 4, 8])))
    # Executes the statement `pay_len = int(rng.integers(8, 24))`.
    pay_len = int(rng.integers(8, 24))
    # Executes the statement `payload_hex = rng.integers(0, 256, pay_len, dtype=np.uint8).tobytes().hex()`.
    payload_hex = rng.integers(0, 256, pay_len, dtype=np.uint8).tobytes().hex()

    # Executes the statement `cmd = [`.
    cmd = [
        # Executes the statement `'python', str(GEN_RANDOM),`.
        'python', str(GEN_RANDOM),
        # Executes the statement `'--output', str(out_dir),`.
        '--output', str(out_dir),
        # Executes the statement `'--sf', str(sf),`.
        '--sf', str(sf),
        # Executes the statement `'--cr', str(cr),`.
        '--cr', str(cr),
        # Executes the statement `'--bw', str(bw),`.
        '--bw', str(bw),
        # Executes the statement `'--sample-rate', str(fs),`.
        '--sample-rate', str(fs),
        # Executes the statement `'--payload-hex', payload_hex,`.
        '--payload-hex', payload_hex,
        # Executes the statement `'--implicit-header',  # will toggle off by removing flag (we want explicit)`.
        '--implicit-header',  # will toggle off by removing flag (we want explicit)
    # Closes the previously opened list indexing or literal.
    ]
    # We want explicit header; do not pass --implicit-header
    # Executes the statement `cmd.remove('--implicit-header')`.
    cmd.remove('--implicit-header')

    # Executes the statement `res = run(cmd)`.
    res = run(cmd)
    # Begins a conditional branch to check a condition.
    if res.returncode != 0:
        # Raises an exception to signal an error.
        raise RuntimeError(f"base gen failed: {res.stderr}\n{res.stdout}")

    # Find last tx_* file
    # Executes the statement `newest = max(out_dir.glob('tx_*.cf32'), key=lambda p: p.stat().st_mtime)`.
    newest = max(out_dir.glob('tx_*.cf32'), key=lambda p: p.stat().st_mtime)
    # Ensure JSON has explicit header and CRC on
    # Executes the statement `meta_path = newest.with_suffix('.json')`.
    meta_path = newest.with_suffix('.json')
    # Executes the statement `meta = json.loads(meta_path.read_text())`.
    meta = json.loads(meta_path.read_text())
    # Executes the statement `meta.update({`.
    meta.update({
        # Executes the statement `'impl_header': False,`.
        'impl_header': False,
        # Executes the statement `'implicit_header': False,`.
        'implicit_header': False,
        # Executes the statement `'crc': True,`.
        'crc': True,
    # Executes the statement `})`.
    })
    # Executes the statement `meta_path.write_text(json.dumps(meta, indent=2))`.
    meta_path.write_text(json.dumps(meta, indent=2))
    # Returns the computed value to the caller.
    return newest


# Defines the function add_noise.
def add_noise(base_cf32: Path, rng: np.random.Generator, out_dir: Path, snr_min: float, snr_max: float) -> Path:
    # Executes the statement `snr_db = float(rng.uniform(snr_min, snr_max))`.
    snr_db = float(rng.uniform(snr_min, snr_max))
    # Executes the statement `cmd = [`.
    cmd = [
        # Executes the statement `'python', str(ADD_AWGN),`.
        'python', str(ADD_AWGN),
        # Executes the statement `str(base_cf32),`.
        str(base_cf32),
        # Executes the statement `'--snr-db', str(round(snr_db, 2)),`.
        '--snr-db', str(round(snr_db, 2)),
        # Executes the statement `'--out-dir', str(out_dir),`.
        '--out-dir', str(out_dir),
    # Closes the previously opened list indexing or literal.
    ]
    # Executes the statement `res = run(cmd)`.
    res = run(cmd)
    # Begins a conditional branch to check a condition.
    if res.returncode != 0:
        # Raises an exception to signal an error.
        raise RuntimeError(f"awgn failed: {res.stderr}\n{res.stdout}")
    # Executes the statement `path = Path(res.stdout.strip().splitlines()[-1])`.
    path = Path(res.stdout.strip().splitlines()[-1])
    # Ensure explicit header flags remain explicit in output JSON
    # Executes the statement `meta_path = path.with_suffix('.json')`.
    meta_path = path.with_suffix('.json')
    # Executes the statement `meta = json.loads(meta_path.read_text())`.
    meta = json.loads(meta_path.read_text())
    # Executes the statement `meta.update({'impl_header': False, 'implicit_header': False, 'crc': True})`.
    meta.update({'impl_header': False, 'implicit_header': False, 'crc': True})
    # Executes the statement `meta_path.write_text(json.dumps(meta, indent=2))`.
    meta_path.write_text(json.dumps(meta, indent=2))
    # Returns the computed value to the caller.
    return path


# Defines the function main.
def main() -> None:
    # Executes the statement `ap = argparse.ArgumentParser(description='Generate explicit-header noisy LoRa vectors set')`.
    ap = argparse.ArgumentParser(description='Generate explicit-header noisy LoRa vectors set')
    # Executes the statement `ap.add_argument('--out', type=Path, default=ROOT / 'golden_vectors' / 'new_batch', help='Output directory')`.
    ap.add_argument('--out', type=Path, default=ROOT / 'golden_vectors' / 'new_batch', help='Output directory')
    # Executes the statement `ap.add_argument('--count', type=int, default=100)`.
    ap.add_argument('--count', type=int, default=100)
    # Executes the statement `ap.add_argument('--seed', type=int, default=42)`.
    ap.add_argument('--seed', type=int, default=42)
    # Executes the statement `ap.add_argument('--snr-min', type=float, default=-20.0)`.
    ap.add_argument('--snr-min', type=float, default=-20.0)
    # Executes the statement `ap.add_argument('--snr-max', type=float, default=10.0)`.
    ap.add_argument('--snr-max', type=float, default=10.0)
    # Executes the statement `args = ap.parse_args()`.
    args = ap.parse_args()

    # Executes the statement `rng = np.random.default_rng(args.seed)`.
    rng = np.random.default_rng(args.seed)
    # Executes the statement `base_dir = args.out / 'base_clean'`.
    base_dir = args.out / 'base_clean'
    # Executes the statement `noisy_dir = args.out`.
    noisy_dir = args.out
    # Executes the statement `base_dir.mkdir(parents=True, exist_ok=True)`.
    base_dir.mkdir(parents=True, exist_ok=True)
    # Executes the statement `noisy_dir.mkdir(parents=True, exist_ok=True)`.
    noisy_dir.mkdir(parents=True, exist_ok=True)

    # Executes the statement `paths: List[Path] = []`.
    paths: List[Path] = []
    # Starts a loop iterating over a sequence.
    for i in range(args.count):
        # Executes the statement `base = generate_base_vector(base_dir, rng)`.
        base = generate_base_vector(base_dir, rng)
        # Executes the statement `noisy = add_noise(base, rng, noisy_dir, args.snr_min, args.snr_max)`.
        noisy = add_noise(base, rng, noisy_dir, args.snr_min, args.snr_max)
        # Executes the statement `paths.append(noisy)`.
        paths.append(noisy)
        # Outputs diagnostic or user-facing text.
        print(f"[gen] {i+1}/{args.count}: {noisy.name}")

    # Outputs diagnostic or user-facing text.
    print(f"Generated {len(paths)} noisy explicit-header vectors in {noisy_dir}")


# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
