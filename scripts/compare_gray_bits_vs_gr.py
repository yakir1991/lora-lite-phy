#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


def gray_decode(v: int) -> int:
    b = 0
    g = v
    while g:
        b ^= g
        g >>= 1
    return b


def read_gr_gray_syms(path: Path, sf: int) -> list[int]:
    b = path.read_bytes()
    # GR dumps appear as 16-bit values per symbol with LSB byte carrying the value
    vals = [b[i] for i in range(0, min(len(b), 2 * 16), 2)]
    return vals[:16]


def main():
    ap = argparse.ArgumentParser(description="Compare Lite vs GR Gray/gnu per header symbol")
    ap.add_argument("--scan", default="logs/lite_hdr_scan.json")
    ap.add_argument("--gr-gray", default="logs/gr_hdr_gray.bin")
    ap.add_argument("--sf", type=int, default=7)
    ap.add_argument("--off0", type=int, default=2)
    ap.add_argument("--samp0", type=int, default=0)
    ap.add_argument("--off1", type=int, default=-1)
    ap.add_argument("--samp1", type=int, default=0)
    ap.add_argument("--out", default="logs/compare_gray_bits_vs_gr.out")
    args = ap.parse_args()

    sf = args.sf
    n = 1 << sf
    sf_app = sf - 2

    gr_syms = read_gr_gray_syms(Path(args.gr_gray), sf)
    # GR Gray decode to binary symbol, then take MSB->LSB bits and gnu5
    gr_bin_syms = [gray_decode(s) & (n - 1) for s in gr_syms]
    gr_bits = [[(b >> (sf - 1 - i)) & 1 for i in range(sf)] for b in gr_bin_syms]
    # Reduce to gnu5 (sf_app bits) for each symbol (per GR header reduction)
    gr_gnu = [((b + n - 1) & (n - 1)) >> 2 for b in gr_bin_syms]

    # Locate the requested hdr-scan entry
    data = json.loads(Path(args.scan).read_text())
    entry = None
    for e in data:
        if (
            e.get("off0") == args.off0
            and e.get("samp0") == args.samp0
            and e.get("off1") == args.off1
            and e.get("samp1") == args.samp1
        ):
            entry = e
            break
    if entry is None:
        raise SystemExit("Requested scan entry not found; adjust --off*/--samp* or rerun hdr-scan")

    raw = entry["syms_raw"]
    lite_bin_syms = [((r + n - 44) & (n - 1)) for r in raw]  # corr used by logging; also compare uncorr
    lite_gray = [((b ^ (b >> 1)) & (n - 1)) for b in lite_bin_syms]
    lite_bits = [[(gray_decode(g) >> (sf - 1 - i)) & 1 for i in range(sf)] for g in lite_gray]
    lite_gnu = [(((r - 1) & (n - 1)) >> 2) for r in raw]

    lines = []
    lines.append("i  | GR_gray  GR_bin  GR_bits        GR_gnu | lite_raw  lite_gnu  delta_raw\n")
    lines.append("---+----------------------------------------+------------------------------\n")
    for i in range(16):
        grg = gr_syms[i]
        grb = gr_bin_syms[i]
        gbits = ''.join(str(x) for x in gr_bits[i])
        ggnu = gr_gnu[i]
        lr = raw[i]
        lgnu = lite_gnu[i]
        delta = (lr - ((4 * ggnu + 1) % n)) % n
        lines.append(
            f"{i:2d} | {grg:7d}  {grb:6d}  {gbits}  {ggnu:6d} | {lr:7d}  {lgnu:7d}  {delta:9d}\n"
        )

    Path(args.out).write_text("".join(lines))
    print(f"Wrote comparison to {args.out}")


if __name__ == "__main__":
    raise SystemExit(main())



