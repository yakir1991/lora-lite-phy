#!/usr/bin/env python3
import argparse, struct, array, os

def read_complex_f32(path):
    with open(path, 'rb') as f:
        data = f.read()
    n = len(data) // 4
    floats = array.array('f')
    floats.frombytes(data)
    # pairs re,im
    return floats

def write_complex_f32(path, floats):
    with open(path, 'wb') as f:
        floats.tofile(f)

def decimate_inplace(path, factor):
    floats = read_complex_f32(path)
    # group into complex pairs
    out = array.array('f')
    # step across complex pairs (2 floats per sample)
    for i in range(0, len(floats), 2*factor):
        if i+1 < len(floats):
            out.append(floats[i])
            out.append(floats[i+1])
    write_complex_f32(path, out)
    return len(out) // 2

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in', dest='inp', required=True)
    ap.add_argument('--factor', type=int, required=True)
    args = ap.parse_args()
    n = decimate_inplace(args.inp, args.factor)
    print(f"[decimate] wrote {n} samples to {args.inp}")

if __name__ == '__main__':
    main()

