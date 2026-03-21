#!/usr/bin/env python3
"""
Brute-force search for a symbol alignment whose first normalized FFT symbols
match a reference prefix (from *_fft.txt).

We drive the existing lora_replay binary with a synthetic sync-offset file for
each candidate and compare the dumped normalized symbols (norm_ldro column)
against the reference prefix. This avoids reimplementing demod in Python.
"""

import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def load_prefix(path: Path, count: int) -> list[int]:
    values: list[int] = []
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                values.append(int(line))
            except ValueError:
                continue
            if len(values) >= count:
                break
    return values


def extract_norm_ldro(path: Path, count: int) -> list[int]:
    vals: list[int] = []
    raw: list[int] = []
    with path.open() as f:
        # skip header line
        header = f.readline()
        if not header:
            return vals
        for line in f:
            parts = line.strip().split()
            if len(parts) < 4:
                continue
            try:
                raw_val = int(parts[1])
                norm_ldro = int(parts[3])
            except ValueError:
                continue
            raw.append(raw_val)
            vals.append(norm_ldro)
            if len(vals) >= count:
                break
    return vals, raw


def run_replay(iq: Path, meta: Path, sync_offset: int, dump_path: Path) -> bool:
    # lora_replay expects a file with offsets; write a single-line file.
    with tempfile.NamedTemporaryFile(mode="w", delete=False) as sync_file:
        sync_file.write(f"{sync_offset}\n")
        sync_path = Path(sync_file.name)
    try:
        cmd = [
            "build/host_sim/lora_replay",
            "--iq",
            str(iq),
            "--metadata",
            str(meta),
            "--sync-offsets",
            str(sync_path),
            "--dump-normalized",
            str(dump_path),
        ]
        result = subprocess.run(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False
        )
        return result.returncode == 0
    finally:
        try:
            sync_path.unlink()
        except Exception:
            pass


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iq", required=True, type=Path)
    parser.add_argument("--metadata", required=True, type=Path)
    parser.add_argument("--reference-fft", required=True, type=Path)
    parser.add_argument("--symbols", type=int, default=8, help="prefix length to match")
    parser.add_argument(
        "--max-symbols",
        type=int,
        default=1200,
        help="maximum symbol index to scan (inclusive)",
    )
    parser.add_argument(
        "--samples-per-symbol",
        type=int,
        default=0,
        help="samples per symbol (SF7/BW125=512)",
    )
    parser.add_argument(
        "--cfo-range",
        type=int,
        default=256,
        help="integer CFO sweep radius (bins)",
    )
    parser.add_argument(
        "--symbol-step",
        type=int,
        default=1,
        help="step size in symbols between scans",
    )
    parser.add_argument(
        "--sample-step",
        type=int,
        default=0,
        help="override sample step between candidates (default = samples_per_symbol)",
    )
    args = parser.parse_args()

    if args.samples_per_symbol <= 0 or args.sample_step <= 0:
        meta = json.load(args.metadata.open())
        sf = meta.get("sf", 7)
        bw = meta.get("bw", 125000)
        fs = meta.get("sample_rate", 500000)
        sps = int(((1 << sf) * fs) // bw)
        if args.samples_per_symbol <= 0:
            args.samples_per_symbol = sps
        if args.sample_step <= 0:
            args.sample_step = sps

    reference_prefix = load_prefix(args.reference_fft, args.symbols)
    if len(reference_prefix) < args.symbols:
        raise SystemExit("Reference FFT file did not contain enough entries")

    dump_path = Path(tempfile.mktemp())
    matches: list[int] = []
    for sym_idx in range(0, args.max_symbols + 1, max(1, args.symbol_step)):
        sync_offset = sym_idx * args.sample_step
        ok = run_replay(args.iq, args.metadata, sync_offset, dump_path)
        if not ok or not dump_path.exists():
            continue
        observed, raw = extract_norm_ldro(dump_path, args.symbols)
        if len(observed) < args.symbols:
            continue
        # Try raw, plus integer CFO adjustments.
        mask = (1 << 12) - 1  # accommodates up to SF12
        candidates = [observed]
        for cfo in range(-args.cfo_range, args.cfo_range + 1):
            adjusted = []
            for val in raw[: args.symbols]:
                shifted = (val - cfo) & mask
                norm = (shifted - 1) & mask
                norm >>= 2
                adjusted.append(norm)
            candidates.append(adjusted)
        if any(c[: args.symbols] == reference_prefix for c in candidates):
            matches.append(sym_idx)
            print(f"[match] symbol={sym_idx} sample_offset={sync_offset}")
    dump_path.unlink(missing_ok=True)

    if not matches:
        print("[match] no matches found")
    else:
        print("[match] candidates:", matches)


if __name__ == "__main__":
    main()
