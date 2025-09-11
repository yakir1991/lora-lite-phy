#!/usr/bin/env python3
import re
from pathlib import Path

def enc48(n: int) -> int:
    d0 = (n >> 0) & 1
    d1 = (n >> 1) & 1
    d2 = (n >> 2) & 1
    d3 = (n >> 3) & 1
    p1 = d0 ^ d1 ^ d3
    p2 = d0 ^ d2 ^ d3
    p3 = d1 ^ d2 ^ d3
    p0 = d0 ^ d1 ^ d2 ^ d3 ^ p1 ^ p2 ^ p3
    return (n & 0xF) | (p1 << 4) | (p2 << 5) | (p3 << 6) | (p0 << 7)

def main():
    lite_log = Path('logs/lite_ld.json')
    gr_nib = Path('logs/gr_hdr_nibbles.bin')
    if not lite_log.exists() or not gr_nib.exists():
        print('missing logs for comparison')
        return 2
    nibs = list(gr_nib.read_bytes()[:10])
    target = [enc48(x & 0xF) for x in nibs]
    print('target cwbytes:', ' '.join(f'{x:02x}' for x in target))
    pat = re.compile(r"cwbytes .*:\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})", re.I)
    best = (999, None)
    for ln in lite_log.read_text(errors='ignore').splitlines():
        m = pat.search(ln)
        if not m: continue
        arr = [int(x,16) for x in m.group(1).split()]
        d = sum(1 for a,b in zip(arr, target) if a != b)
        if d < best[0]:
            best = (d, ln.strip())
    print('best distance to GR cwbytes:', best[0])
    if best[1]:
        print(best[1])
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

