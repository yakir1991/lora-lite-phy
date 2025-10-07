#!/usr/bin/env python3
"""Aggregate diagnostics from header slice meta sidecars.

Scans a directory for *_header.cf32.meta.json files and summarizes:
- Count, header_ok ratio
- CFO used distribution (min/max/mean)
- Candidate offset samples histogram
- By (sf,bw) grouping, quick success counts

Optionally writes a CSV with one row per file.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple


@dataclass
class Entry:
    path: Path
    sf: int
    bw: int
    fs: int
    cr: int
    has_crc: int
    impl_header: int
    ldro_mode: int
    cfo_used_hz: float
    p_ofs_est: int
    cand_offset_samples: int
    attempts: int
    header_ok: int


def load_meta(p: Path) -> Entry | None:
    try:
        d = json.loads(p.read_text())
        return Entry(
            path=p,
            sf=int(d.get('sf', 0)),
            bw=int(d.get('bw', 0)),
            fs=int(d.get('fs', d.get('samp_rate', d.get('sample_rate', 0)))),
            cr=int(d.get('cr', 0)),
            has_crc=int(d.get('has_crc', 0)),
            impl_header=int(d.get('impl_header', 0)),
            ldro_mode=int(d.get('ldro_mode', 0)),
            cfo_used_hz=float(d.get('cfo_used_hz', 0.0)),
            p_ofs_est=int(d.get('p_ofs_est', 0)),
            cand_offset_samples=int(d.get('cand_offset_samples', 0)),
            attempts=int(d.get('attempts', 0)),
            header_ok=int(d.get('header_ok', 0)),
        )
    except Exception:
        return None


def main() -> None:
    ap = argparse.ArgumentParser(description='Analyze header slice meta sidecars')
    ap.add_argument('--dir', type=Path, default=Path('results') / 'hdr_slices')
    ap.add_argument('--csv', type=Path, default=None)
    args = ap.parse_args()

    metas = sorted(args.dir.glob('*_header.cf32.meta.json'))
    entries: List[Entry] = []
    for m in metas:
        e = load_meta(m)
        if e:
            entries.append(e)

    if not entries:
        print('No meta files found in', args.dir)
        return

    total = len(entries)
    ok = sum(e.header_ok for e in entries)
    cfo_vals = [e.cfo_used_hz for e in entries]
    min_cfo = min(cfo_vals)
    max_cfo = max(cfo_vals)
    mean_cfo = sum(cfo_vals) / len(cfo_vals)
    # Histogram of candidate offsets in symbols (approx)
    ofs_hist: Dict[int, int] = {}
    for e in entries:
        ofs_hist[e.cand_offset_samples] = ofs_hist.get(e.cand_offset_samples, 0) + 1

    # Group by (sf,bw)
    groups: Dict[Tuple[int, int], Tuple[int, int]] = {}
    for e in entries:
        key = (e.sf, e.bw)
        c = groups.get(key, (0, 0))
        groups[key] = (c[0] + 1, c[1] + (1 if e.header_ok else 0))

    print(f"Files: {total}  header_ok: {ok} ({ok/total*100:.1f}%)")
    print(f"CFO used (Hz): min={min_cfo:.1f} max={max_cfo:.1f} mean={mean_cfo:.1f}")
    print("Candidate offset samples histogram:")
    for k in sorted(ofs_hist):
        print(f"  ofs={k:6d} : {ofs_hist[k]}")
    print("By (sf,bw):")
    for (sf, bw), (cnt, okc) in sorted(groups.items()):
        print(f"  sf{sf} bw{bw}: {okc}/{cnt} ({(okc/cnt*100) if cnt else 0:.1f}%)")

    if args.csv:
        import csv
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open('w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['path','sf','bw','fs','cr','has_crc','impl_header','ldro_mode','cfo_used_hz','p_ofs_est','cand_offset_samples','attempts','header_ok'])
            for e in entries:
                w.writerow([str(e.path), e.sf, e.bw, e.fs, e.cr, e.has_crc, e.impl_header, e.ldro_mode, f"{e.cfo_used_hz:.3f}", e.p_ofs_est, e.cand_offset_samples, e.attempts, e.header_ok])
        print('Wrote CSV to', args.csv)


if __name__ == '__main__':
    main()
