#!/usr/bin/env python3
from pathlib import Path

def main():
    p = Path('logs/gr_deint_bits.bin')
    if not p.exists():
        print('missing logs/gr_deint_bits.bin')
        return 2
    b = p.read_bytes()
    target = bytes([0x00,0x74,0xC5,0x00,0xC5,0x1D,0x12,0x1B,0x12,0x00])
    idx = []
    for i in range(0, len(b) - len(target) + 1):
        if b[i:i+10] == target:
            idx.append(i)
    print('occurrences', len(idx))
    print('indices', idx[:10])
    return 0

if __name__=='__main__':
    raise SystemExit(main())

