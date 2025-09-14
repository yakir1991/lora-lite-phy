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


def rows_to_inter(rows_bytes, sf_app: int):
    # rows_bytes: 5 bytes (MSB-first) representing deinter rows
    # returns inter[8][sf_app] with bits per column (MSB->LSB)
    rows = [[(rows_bytes[r] >> (7 - i)) & 1 for i in range(8)] for r in range(sf_app)]
    inter = [[0] * sf_app for _ in range(8)]
    for i in range(8):
        for j in range(sf_app):
            r = (i - j - 1) % sf_app
            inter[i][j] = rows[r][i]
    return inter


def derive_gr_gnu_from_cw(cw_bytes, sf: int):
    sf_app = sf - 2
    inter0 = rows_to_inter(cw_bytes[:5], sf_app)
    inter1 = rows_to_inter(cw_bytes[5:10], sf_app)
    gray0 = [int("".join(str(inter0[i][j]) for j in range(sf_app)), 2) for i in range(8)]
    gray1 = [int("".join(str(inter1[i][j]) for j in range(sf_app)), 2) for i in range(8)]
    gnu0 = [gray_decode(v) & ((1 << sf_app) - 1) for v in gray0]
    gnu1 = [gray_decode(v) & ((1 << sf_app) - 1) for v in gray1]
    return gnu0, gnu1


def compute_gnu_from_raw_syms(raw_syms, sf: int):
    # raw_syms: list of 16 raw FFT bins (0..N-1)
    N = 1 << sf
    gnu0 = [(((r - 1) & (N - 1)) >> 2) for r in raw_syms[:8]]
    gnu1 = [(((r - 1) & (N - 1)) >> 2) for r in raw_syms[8:16]]
    return gnu0, gnu1


def main():
    ap = argparse.ArgumentParser(description="Scan hdr-scan JSON for matches to GR-derived gnu vectors")
    ap.add_argument("--scan", default="logs/lite_hdr_scan.json", help="Path to hdr-scan JSON")
    ap.add_argument("--gr-cw", default="logs/gr_deint_bits.bin", help="Path to GR CW bytes (10 bytes)")
    ap.add_argument("--sf", type=int, default=7, help="Spreading factor (default: 7)")
    ap.add_argument("--out", default="logs/gnu_match_scan.out", help="Output summary file")
    args = ap.parse_args()

    scan_path = Path(args.scan)
    gr_path = Path(args.gr_cw)
    if not scan_path.exists():
        raise SystemExit(f"scan file not found: {scan_path}")

    # Read GR CW bytes (10) or fallback to known SF7 canonical
    if gr_path.exists():
        cw = list(gr_path.read_bytes()[:10])
    else:
        if args.sf != 7:
            raise SystemExit("Missing GR CW file and no fallback known for sf!=7")
        cw = [0x00, 0x74, 0xC5, 0x00, 0xC5, 0x1D, 0x12, 0x1B, 0x12, 0x00]

    gnu0_gr, gnu1_gr = derive_gr_gnu_from_cw(cw, args.sf)

    data = json.loads(scan_path.read_text())
    exact_b1 = None
    exact_both = None
    best_b1 = (999, None, None)  # (diff, entry, gnu1)
    best_both = (999, None, None)  # (sum diff0+diff1, entry, (gnu0, gnu1))

    for e in data:
        raw = e.get("syms_raw", [])
        if len(raw) < 16:
            continue
        gnu0, gnu1 = compute_gnu_from_raw_syms(raw, args.sf)
        d1 = sum(1 for a, b in zip(gnu1, gnu1_gr) if a != b)
        d0 = sum(1 for a, b in zip(gnu0, gnu0_gr) if a != b)
        if d1 == 0 and exact_b1 is None:
            exact_b1 = e
        if d1 == 0 and d0 == 0:
            exact_both = e
            break
        if d1 < best_b1[0]:
            best_b1 = (d1, e, gnu1)
        tot = d0 + d1
        if tot < best_both[0]:
            best_both = (tot, e, (gnu0, gnu1))

    lines = []
    lines.append(f"GR_GNU0 {gnu0_gr}\n")
    lines.append(f"GR_GNU1 {gnu1_gr}\n")
    lines.append(f"exact_block1 {exact_b1 is not None}\n")
    if exact_b1:
        lines.append(
            f"b1 params {{'off0':{exact_b1['off0']},'samp0':{exact_b1['samp0']},'off1':{exact_b1['off1']},'samp1':{exact_b1['samp1']}}}\n"
        )
    lines.append(f"exact_both {exact_both is not None}\n")
    if exact_both:
        lines.append(
            f"both params {{'off0':{exact_both['off0']},'samp0':{exact_both['samp0']},'off1':{exact_both['off1']},'samp1':{exact_both['samp1']}}}\n"
        )
    lines.append(f"best_b1_diff {best_b1[0]}\n")
    if best_b1[1]:
        e = best_b1[1]
        lines.append(
            f"best_b1_params {{'off0':{e['off0']},'samp0':{e['samp0']},'off1':{e['off1']},'samp1':{e['samp1']}}}\n"
        )
        lines.append(f"best_b1_gnu {best_b1[2]}\n")
    lines.append(f"best_both_totdiff {best_both[0]}\n")
    if best_both[1]:
        e = best_both[1]
        lines.append(
            f"best_both_params {{'off0':{e['off0']},'samp0':{e['samp0']},'off1':{e['off1']},'samp1':{e['samp1']}}}\n"
        )

    outp = Path(args.out)
    outp.write_text("".join(lines))
    print(f"Wrote summary to {outp}")


if __name__ == "__main__":
    raise SystemExit(main())


