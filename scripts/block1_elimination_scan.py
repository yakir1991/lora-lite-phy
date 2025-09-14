#!/usr/bin/env python3
import argparse
import json
import sys
from typing import List, Dict, Any


def gray_encode(x: int) -> int:
    return x ^ (x >> 1)


def build_cw_from_gnu(gnu_vals: List[int], sf: int, use_gray: bool) -> List[int]:
    """
    Rebuild 5 row-bytes (CW rows) for one 8-symbol block from gnu values
    using the same mapping as the C++ build_block_rows:
      - sf_app = sf-2 bits per symbol
      - inter_bin[i][j] from MSB->LSB of sub = (Gray(full) & ((1<<sf_app)-1)) if use_gray else (full & ...)
      - deinterleave: r = (i - j - 1) mod sf_app
      - rows→bytes: MSB-first across 8 columns
    """
    sf_app = sf - 2
    inter = [[0 for _ in range(sf_app)] for _ in range(8)]
    mask = (1 << sf_app) - 1
    for i in range(8):
        full = gnu_vals[i]
        g = gray_encode(full) if use_gray else full
        sub = g & mask
        for j in range(sf_app):
            # MSB-first to LSB
            bit = (sub >> (sf_app - 1 - j)) & 1
            inter[i][j] = bit
    deint = [[0 for _ in range(8)] for _ in range(sf_app)]
    for i in range(8):
        for j in range(sf_app):
            r = (i - j - 1) % sf_app
            deint[r][i] = inter[i][j]
    rows = []
    for r in range(sf_app):
        c = 0
        for i in range(8):
            c = (c << 1) | (deint[r][i] & 1)
        rows.append(c)
    return rows


def gnu_from_raw_syms(syms_raw: List[int], sf: int) -> List[int]:
    N = 1 << sf
    return [((rv + N - 1) & (N - 1)) >> 2 for rv in syms_raw]


def hex_bytes(arr: List[int]) -> str:
    return " ".join(f"{b:02x}" for b in arr)


def parse_gr_cw(s: str) -> List[int]:
    toks = [t for t in s.strip().replace(",", " ").split() if t]
    out = []
    for t in toks:
        out.append(int(t, 16))
    if len(out) != 10:
        raise ValueError("--gr-cw must have exactly 10 bytes (CW rows)")
    return out


def count_byte_mismatches(a: List[int], b: List[int]) -> int:
    return sum(1 for i in range(min(len(a), len(b))) if a[i] != b[i])


def main() -> int:
    ap = argparse.ArgumentParser(description="Block-1 elimination scan over hdr-scan syms_raw")
    ap.add_argument("--scan", required=True, help="Path to logs/lite_hdr_scan.json (from --hdr-scan)")
    ap.add_argument("--sf", type=int, required=True, help="Spreading factor (e.g., 7)")
    ap.add_argument("--gr-cw", required=True, help="GR ground-truth CW bytes (10 bytes hex, space-separated)")
    ap.add_argument("--out", required=True, help="Output JSON report path")
    ap.add_argument("--limit", type=int, default=200, help="Max number of scan entries to evaluate (default: 200)")
    args = ap.parse_args()

    # Load scan JSON
    with open(args.scan, "r") as f:
        scan = json.load(f)
    if not isinstance(scan, list):
        print("scan JSON must be a list", file=sys.stderr)
        return 2
    entries = scan[: args.limit]
    print(f"Loaded {len(entries)} entries from {args.scan}")

    gr_cw = parse_gr_cw(args.gr_cw)
    sf = args.sf
    N = 1 << sf
    if sf < 5:
        print("sf too small for header (sf_app=sf-2)", file=sys.stderr)
        return 2

    best_by_variant: Dict[str, Dict[str, Any]] = {}

    def consider(variant: str, rec: Dict[str, Any]):
        prev = best_by_variant.get(variant)
        if prev is None or rec["diff_full"] < prev["diff_full"]:
            best_by_variant[variant] = rec

    for idx, e in enumerate(entries):
        off0 = e.get("off0"); samp0 = e.get("samp0"); off1 = e.get("off1"); samp1 = e.get("samp1")
        syms_raw = e.get("syms_raw")
        if not isinstance(syms_raw, list) or len(syms_raw) != 16:
            continue
        gnu_vals = gnu_from_raw_syms(syms_raw, sf)

        # Block-0 always standard (Gray on)
        cw0 = build_cw_from_gnu(gnu_vals[0:8], sf, use_gray=True)

        # Variants for Block-1: Gray on/off
        variants = {
            "b1_gray_on": True,
            "b1_gray_off": False,
        }

        for vname, vgray in variants.items():
            cw1 = build_cw_from_gnu(gnu_vals[8:16], sf, use_gray=vgray)
            cw = cw0 + cw1
            diff_full = count_byte_mismatches(cw, gr_cw)
            diff_b1 = count_byte_mismatches(cw[5:], gr_cw[5:])
            rec = {
                "off0": off0, "samp0": samp0, "off1": off1, "samp1": samp1,
                "variant": vname,
                "cw_hex": hex_bytes(cw),
                "diff_full": diff_full,
                "diff_block1": diff_b1,
            }
            consider(vname, rec)

        if (idx + 1) % 25 == 0 or idx + 1 == len(entries):
            print(f"Progress: {idx+1}/{len(entries)} evaluated…")

    report = {
        "sf": sf,
        "scan": args.scan,
        "limit": args.limit,
        "gr_cw": args.gr_cw,
        "best": best_by_variant,
        "notes": [
            "This scan tests Block-1 Gray mapping on/off at CW level.",
            "Header whitening and CRC do not affect CW bytes; they are not applied here.",
        ],
    }
    with open(args.out, "w") as f:
        json.dump(report, f, indent=2)
    print(f"Wrote report to {args.out}")
    # Pretty-print best summary to stdout
    for k in sorted(best_by_variant.keys()):
        r = best_by_variant[k]
        print(f"best[{k}]: diff_full={r['diff_full']} diff_block1={r['diff_block1']} at off1={r['off1']} samp1={r['samp1']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())


