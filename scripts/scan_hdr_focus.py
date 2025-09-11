#!/usr/bin/env python3
import re
from pathlib import Path

TARGET_PATH = Path('logs/gr_deint_bits.bin')
LITE_LOG = Path('logs/lite_ld.json')

PATS = {
    '1blk': re.compile(r"hdr_gr cwbytes \(samp=([\-\d]+) off=(\d+) mode=(raw|corr)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I),
    '2blk': re.compile(r"hdr_gr cwbytes \(2blk samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I),
    '2blk-col': re.compile(r"hdr_gr cwbytes \(2blk-col samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) colrev=(\d+) rowrev=(\d+) swap=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I),
    '2blk-var': re.compile(r"hdr_gr cwbytes \(2blk-var samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) rot1=(\d+) rowrev1=(\d+) colrev1=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I),
    '2blk-varshift': re.compile(r"hdr_gr cwbytes \(2blk-varshift samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) rot1=(\d+) rowrev1=(\d+) colshift1=(\d+) colrev1=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I),
    '2blk-var2': re.compile(r"hdr_gr cwbytes \(2blk-var2 samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) altdiag=1 colrev1=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I),
    '2blk-diagvar': re.compile(r"hdr_gr cwbytes \(2blk-diagvar samp0=([\-\d]+) off0=(\d+) samp1=([\-\d]+) off1=(\d+) mode=(raw|corr) diagshift=(\-?\d+) rot1=(\d+) rowrev1=(\d+) colshift1=(\d+) colrev1=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I),
}

def load_target():
    b = TARGET_PATH.read_bytes()
    return [x for x in b[:10]]

def parse_lite_lines(target):
    entries = []
    for ln in LITE_LOG.read_text(errors='ignore').splitlines():
        for kind, rg in PATS.items():
            m = rg.search(ln)
            if not m:
                continue
            vals = [int(x,16) for x in m.group(m.lastindex).split()]
            full_diff = sum(1 for a,b in zip(vals, target) if a!=b)
            last5_diff = sum(1 for a,b in zip(vals[5:], target[5:]) if a!=b)
            entries.append((full_diff, last5_diff, kind, ln.strip()))
            break
    return entries

def main():
    if not TARGET_PATH.exists():
        print('Missing target', TARGET_PATH)
        return 2
    if not LITE_LOG.exists():
        print('Missing lite log', LITE_LOG)
        return 2
    tgt = load_target()
    entries = parse_lite_lines(tgt)
    if not entries:
        print('No hdr_gr cwbytes lines found in', LITE_LOG)
        return 1
    entries.sort(key=lambda x: (x[0], x[1]))
    print('Top 12 candidates (by full_diff, then last5_diff):')
    for i,(fd, ld, kind, ln) in enumerate(entries[:12], 1):
        print(f'{i:2d}) full={fd} last5={ld} kind={kind} | {ln}')
    # Group by kind best
    print('\nBest per kind:')
    best_by = {}
    for fd, ld, kind, ln in entries:
        if kind not in best_by or (fd,ld) < best_by[kind][0]:
            best_by[kind] = ((fd, ld), ln)
    for kind, ((fd,ld), ln) in best_by.items():
        print(f'- {kind}: full={fd} last5={ld} | {ln}')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

