#!/usr/bin/env python3
from pathlib import Path
import sys

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
    p = Path('logs/gr_hdr_nibbles.bin')
    if not p.exists():
        print('missing logs/gr_hdr_nibbles.bin', file=sys.stderr)
        return 2
    data = p.read_bytes()
    nibs = list(data[:10])
    print('nibbles:', ' '.join(f'{x:01x}' for x in nibs))
    print('cwbytes:', ' '.join(f'{enc48(x):02x}' for x in nibs))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

