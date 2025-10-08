#!/usr/bin/env python3
# This file provides the 'analyze header slice meta' functionality for the LoRa Lite PHY toolkit.
"""Aggregate diagnostics from header slice meta sidecars.

Scans a directory for *_header.cf32.meta.json files and summarizes:
- Count, header_ok ratio
- CFO used distribution (min/max/mean)
- Candidate offset samples histogram
- By (sf,bw) grouping, quick success counts

Optionally writes a CSV with one row per file.
"""

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports specific objects with 'from dataclasses import dataclass'.
from dataclasses import dataclass
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from typing import Dict, List, Tuple'.
from typing import Dict, List, Tuple


# Executes the statement `@dataclass`.
@dataclass
# Declares the class Entry.
class Entry:
    # Executes the statement `path: Path`.
    path: Path
    # Executes the statement `sf: int`.
    sf: int
    # Executes the statement `bw: int`.
    bw: int
    # Executes the statement `fs: int`.
    fs: int
    # Executes the statement `cr: int`.
    cr: int
    # Executes the statement `has_crc: int`.
    has_crc: int
    # Executes the statement `impl_header: int`.
    impl_header: int
    # Executes the statement `ldro_mode: int`.
    ldro_mode: int
    # Executes the statement `cfo_used_hz: float`.
    cfo_used_hz: float
    # Executes the statement `p_ofs_est: int`.
    p_ofs_est: int
    # Executes the statement `cand_offset_samples: int`.
    cand_offset_samples: int
    # Executes the statement `attempts: int`.
    attempts: int
    # Executes the statement `header_ok: int`.
    header_ok: int


# Defines the function load_meta.
def load_meta(p: Path) -> Entry | None:
    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `d = json.loads(p.read_text())`.
        d = json.loads(p.read_text())
        # Returns the computed value to the caller.
        return Entry(
            # Executes the statement `path=p,`.
            path=p,
            # Executes the statement `sf=int(d.get('sf', 0)),`.
            sf=int(d.get('sf', 0)),
            # Executes the statement `bw=int(d.get('bw', 0)),`.
            bw=int(d.get('bw', 0)),
            # Executes the statement `fs=int(d.get('fs', d.get('samp_rate', d.get('sample_rate', 0)))),`.
            fs=int(d.get('fs', d.get('samp_rate', d.get('sample_rate', 0)))),
            # Executes the statement `cr=int(d.get('cr', 0)),`.
            cr=int(d.get('cr', 0)),
            # Executes the statement `has_crc=int(d.get('has_crc', 0)),`.
            has_crc=int(d.get('has_crc', 0)),
            # Executes the statement `impl_header=int(d.get('impl_header', 0)),`.
            impl_header=int(d.get('impl_header', 0)),
            # Executes the statement `ldro_mode=int(d.get('ldro_mode', 0)),`.
            ldro_mode=int(d.get('ldro_mode', 0)),
            # Executes the statement `cfo_used_hz=float(d.get('cfo_used_hz', 0.0)),`.
            cfo_used_hz=float(d.get('cfo_used_hz', 0.0)),
            # Executes the statement `p_ofs_est=int(d.get('p_ofs_est', 0)),`.
            p_ofs_est=int(d.get('p_ofs_est', 0)),
            # Executes the statement `cand_offset_samples=int(d.get('cand_offset_samples', 0)),`.
            cand_offset_samples=int(d.get('cand_offset_samples', 0)),
            # Executes the statement `attempts=int(d.get('attempts', 0)),`.
            attempts=int(d.get('attempts', 0)),
            # Executes the statement `header_ok=int(d.get('header_ok', 0)),`.
            header_ok=int(d.get('header_ok', 0)),
        # Closes the previously opened parenthesis grouping.
        )
    # Handles a specific exception from the try block.
    except Exception:
        # Returns the computed value to the caller.
        return None


# Defines the function main.
def main() -> None:
    # Executes the statement `ap = argparse.ArgumentParser(description='Analyze header slice meta sidecars')`.
    ap = argparse.ArgumentParser(description='Analyze header slice meta sidecars')
    # Executes the statement `ap.add_argument('--dir', type=Path, default=Path('results') / 'hdr_slices')`.
    ap.add_argument('--dir', type=Path, default=Path('results') / 'hdr_slices')
    # Executes the statement `ap.add_argument('--csv', type=Path, default=None)`.
    ap.add_argument('--csv', type=Path, default=None)
    # Executes the statement `args = ap.parse_args()`.
    args = ap.parse_args()

    # Executes the statement `metas = sorted(args.dir.glob('*_header.cf32.meta.json'))`.
    metas = sorted(args.dir.glob('*_header.cf32.meta.json'))
    # Executes the statement `entries: List[Entry] = []`.
    entries: List[Entry] = []
    # Starts a loop iterating over a sequence.
    for m in metas:
        # Executes the statement `e = load_meta(m)`.
        e = load_meta(m)
        # Begins a conditional branch to check a condition.
        if e:
            # Executes the statement `entries.append(e)`.
            entries.append(e)

    # Begins a conditional branch to check a condition.
    if not entries:
        # Outputs diagnostic or user-facing text.
        print('No meta files found in', args.dir)
        # Returns control to the caller.
        return

    # Executes the statement `total = len(entries)`.
    total = len(entries)
    # Executes the statement `ok = sum(e.header_ok for e in entries)`.
    ok = sum(e.header_ok for e in entries)
    # Executes the statement `cfo_vals = [e.cfo_used_hz for e in entries]`.
    cfo_vals = [e.cfo_used_hz for e in entries]
    # Executes the statement `min_cfo = min(cfo_vals)`.
    min_cfo = min(cfo_vals)
    # Executes the statement `max_cfo = max(cfo_vals)`.
    max_cfo = max(cfo_vals)
    # Executes the statement `mean_cfo = sum(cfo_vals) / len(cfo_vals)`.
    mean_cfo = sum(cfo_vals) / len(cfo_vals)
    # Histogram of candidate offsets in symbols (approx)
    # Executes the statement `ofs_hist: Dict[int, int] = {}`.
    ofs_hist: Dict[int, int] = {}
    # Starts a loop iterating over a sequence.
    for e in entries:
        # Executes the statement `ofs_hist[e.cand_offset_samples] = ofs_hist.get(e.cand_offset_samples, 0) + 1`.
        ofs_hist[e.cand_offset_samples] = ofs_hist.get(e.cand_offset_samples, 0) + 1

    # Group by (sf,bw)
    # Executes the statement `groups: Dict[Tuple[int, int], Tuple[int, int]] = {}`.
    groups: Dict[Tuple[int, int], Tuple[int, int]] = {}
    # Starts a loop iterating over a sequence.
    for e in entries:
        # Executes the statement `key = (e.sf, e.bw)`.
        key = (e.sf, e.bw)
        # Executes the statement `c = groups.get(key, (0, 0))`.
        c = groups.get(key, (0, 0))
        # Executes the statement `groups[key] = (c[0] + 1, c[1] + (1 if e.header_ok else 0))`.
        groups[key] = (c[0] + 1, c[1] + (1 if e.header_ok else 0))

    # Outputs diagnostic or user-facing text.
    print(f"Files: {total}  header_ok: {ok} ({ok/total*100:.1f}%)")
    # Outputs diagnostic or user-facing text.
    print(f"CFO used (Hz): min={min_cfo:.1f} max={max_cfo:.1f} mean={mean_cfo:.1f}")
    # Outputs diagnostic or user-facing text.
    print("Candidate offset samples histogram:")
    # Starts a loop iterating over a sequence.
    for k in sorted(ofs_hist):
        # Outputs diagnostic or user-facing text.
        print(f"  ofs={k:6d} : {ofs_hist[k]}")
    # Outputs diagnostic or user-facing text.
    print("By (sf,bw):")
    # Starts a loop iterating over a sequence.
    for (sf, bw), (cnt, okc) in sorted(groups.items()):
        # Outputs diagnostic or user-facing text.
        print(f"  sf{sf} bw{bw}: {okc}/{cnt} ({(okc/cnt*100) if cnt else 0:.1f}%)")

    # Begins a conditional branch to check a condition.
    if args.csv:
        # Imports the module(s) csv.
        import csv
        # Accesses a parsed command-line argument.
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        # Opens a context manager scope for managed resources.
        with args.csv.open('w', newline='') as f:
            # Executes the statement `w = csv.writer(f)`.
            w = csv.writer(f)
            # Executes the statement `w.writerow(['path','sf','bw','fs','cr','has_crc','impl_header','ldro_mode','cfo_used_hz','p_ofs_est','cand_offset_samples','attempts','header_ok'])`.
            w.writerow(['path','sf','bw','fs','cr','has_crc','impl_header','ldro_mode','cfo_used_hz','p_ofs_est','cand_offset_samples','attempts','header_ok'])
            # Starts a loop iterating over a sequence.
            for e in entries:
                # Executes the statement `w.writerow([str(e.path), e.sf, e.bw, e.fs, e.cr, e.has_crc, e.impl_header, e.ldro_mode, f"{e.cfo_used_hz:.3f}", e.p_ofs_est, e.cand_offset_samples, e.attempts, e.header_ok])`.
                w.writerow([str(e.path), e.sf, e.bw, e.fs, e.cr, e.has_crc, e.impl_header, e.ldro_mode, f"{e.cfo_used_hz:.3f}", e.p_ofs_est, e.cand_offset_samples, e.attempts, e.header_ok])
        # Outputs diagnostic or user-facing text.
        print('Wrote CSV to', args.csv)


# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
