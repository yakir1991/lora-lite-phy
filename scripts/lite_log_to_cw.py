#!/usr/bin/env python3
"""Extract header codewords from LoRa Lite logs and optionally compare with GNU Radio."""
import argparse, json, re
from pathlib import Path

SF = 7
N = 1 << SF
SF_APP = SF - 2

def parse_last_json(path: Path):
    txt = path.read_text(errors='ignore')
    objs = re.findall(r"\{[\s\S]*?\}\s*$", txt, re.M)
    if objs:
        for obj in reversed(objs):
            try:
                return json.loads(obj)
            except Exception:
                continue
    try:
        return json.loads(txt)
    except Exception:
        return None

def parse_debug_gray_from_log(path: Path):
    txt = path.read_text(errors='ignore').splitlines()
    pat = re.compile(r"^DEBUG: Header symbol\s+(\d+):\s+raw=\d+,\s+corr=\d+,\s+gray=(\d+)")
    syms = {}
    for ln in txt:
        m = pat.match(ln.strip())
        if not m:
            continue
        k = int(m.group(1))
        g = int(m.group(2))
        if 0 <= k < 16:
            syms[k] = g
    if len(syms) >= 16:
        return [syms[i] for i in range(16)]
    return None

def parse_debug_raw_from_log(path: Path):
    txt = path.read_text(errors='ignore').splitlines()
    pat = re.compile(r"^DEBUG: Header symbol\s+(\d+):\s+raw=(\d+),\s+corr=\d+,\s+gray=\d+")
    syms = {}
    for ln in txt:
        m = pat.match(ln.strip())
        if not m:
            continue
        k = int(m.group(1))
        r = int(m.group(2))
        if 0 <= k < 16:
            syms[k] = r
    if len(syms) >= 16:
        return [syms[i] for i in range(16)]
    return None

def gray_encode(x: int, bits: int = 7) -> int:
    return (x ^ (x >> 1)) & ((1 << bits) - 1)

def rx_cw_from_gray(vals: list[int]) -> list[int]:
    cw_all = []
    cw_len = 8
    for blk in range(2):
        inter_bin = [[0] * SF_APP for _ in range(cw_len)]
        for i in range(cw_len):
            full = vals[blk * cw_len + i] & (N - 1)
            sub = full & ((1 << SF_APP) - 1)
            for j in range(SF_APP):
                inter_bin[i][j] = (sub >> (SF_APP - 1 - j)) & 1
        deinter_bin = [[0] * cw_len for _ in range(SF_APP)]
        for i in range(cw_len):
            for j in range(SF_APP):
                r = (i - j - 1) % SF_APP
                deinter_bin[r][i] = inter_bin[i][j]
        for r in range(SF_APP):
            cw = 0
            for c in range(cw_len):
                cw = (cw << 1) | (deinter_bin[r][c] & 1)
            cw_all.append(cw)
    return cw_all[:10]

def extract_gray_vals(path: Path) -> list[int]:
    obj = parse_last_json(path)
    if obj:
        if isinstance(obj.get('syms_gray'), list) and len(obj['syms_gray']) >= 16:
            return obj['syms_gray'][:16]
        if isinstance(obj.get('syms_raw'), list) and len(obj['syms_raw']) >= 16:
            raw = obj['syms_raw'][:16]
            return [gray_encode(((r - 1) & (N - 1)) >> 2, bits=SF_APP) for r in raw]
    g = parse_debug_gray_from_log(path)
    if g:
        return g
    r = parse_debug_raw_from_log(path)
    if r:
        return [gray_encode(((x - 1) & (N - 1)) >> 2, bits=SF_APP) for x in r]
    raise RuntimeError('Unable to extract header symbols from log')

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

def read_gr_cw(path: Path) -> list[int]:
    nibs = list(path.read_bytes()[:10])
    return [enc48(x & 0xF) for x in nibs]

def main() -> int:
    ap = argparse.ArgumentParser(description='Convert Lite logs to header CW bytes')
    ap.add_argument('--log', default='logs/lite_ld.json', help='path to lite log file')
    ap.add_argument('--gr-nibbles', help='optional GNU Radio header nibbles file for comparison')
    args = ap.parse_args()

    log_path = Path(args.log)
    if not log_path.exists():
        print(f'missing log file: {log_path}')
        return 1
    gvals = extract_gray_vals(log_path)
    cw = rx_cw_from_gray(gvals)
    print('lite cw :', ' '.join(f'{x:02x}' for x in cw))
    if args.gr_nibbles:
        gr_path = Path(args.gr_nibbles)
        if gr_path.exists():
            target = read_gr_cw(gr_path)
            diff = sum(1 for a, b in zip(cw, target) if a != b)
            print('gr   cw :', ' '.join(f'{x:02x}' for x in target))
            print('diff    :', diff)
        else:
            print(f'missing GNU Radio nibbles file: {gr_path}')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
