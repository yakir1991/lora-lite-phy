#!/usr/bin/env python3
import argparse, re, sys

TARGET = [0x00, 0x74, 0xC5, 0x00, 0xC5, 0x1D, 0x12, 0x1B, 0x12, 0x00]

def parse_line(line: str):
    m = re.search(r"hdr_map cwbytes \(msb=(\-?\d+) sign=(\-?\d+) extra=(\d+) col=(\d+)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})",
                  line.strip(), re.IGNORECASE)
    if not m:
        return None
    msb = int(m.group(1)); sign = int(m.group(2)); extra = int(m.group(3)); col = int(m.group(4))
    bytestr = m.group(5)
    vals = [int(x, 16) for x in bytestr.split()]
    return (msb, sign, extra, col, vals)

def dist(a, b):
    return sum(int(x != y) for x, y in zip(a, b))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log', help='lite run output file with cwbytes lines (e.g., logs/lite_run_latest5.out)')
    args = ap.parse_args()
    best = (999, None)
    for line in open(args.log, 'r', errors='ignore'):
        rec = parse_line(line)
        if not rec: continue
        d = dist(rec[4], TARGET)
        if d < best[0]:
            best = (d, rec)
    if not best[1]:
        print('no cwbytes found'); return 1
    d, (msb, sign, extra, col, vals) = best
    print('best dist', d, 'params', {'msb':msb,'sign':sign,'extra':extra,'col':col}, 'cw', ' '.join(f'{v:02x}' for v in vals))
    print('target    ', ' '.join(f'{v:02x}' for v in TARGET))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

