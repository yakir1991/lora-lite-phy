#!/usr/bin/env python3
import argparse, struct

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in', dest='inp', required=True)
    ap.add_argument('--out', dest='outp', required=True)
    ap.add_argument('--tail-floats', type=int, default=2048, help='number of float32 values to zero at end (I+Q interleaved)')
    args = ap.parse_args()

    with open(args.inp, 'rb') as f:
        data = bytearray(f.read())
    n_floats = len(data)//4
    zero_floats = min(args.tail_floats, n_floats)
    start = (n_floats - zero_floats) * 4
    for i in range(start, len(data), 4):
        data[i:i+4] = b'\x00\x00\x00\x00'
    with open(args.outp, 'wb') as f:
        f.write(data)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

