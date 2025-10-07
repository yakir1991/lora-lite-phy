#!/usr/bin/env python3
"""Analyze streaming_compat_results.json and print grouped stats.

Groups by (sf, bw, cr) and SNR bins, reporting:
 - total, cpp_success_count, match_count
 - header decode failures vs payload failures (heuristic via stderr)
"""

from __future__ import annotations

import json
from pathlib import Path
import argparse
from collections import defaultdict
from typing import Dict, Any, Tuple

ROOT = Path(__file__).resolve().parents[1]
RESULTS_JSON = ROOT / 'results' / 'streaming_compat_results.json'

def snr_bin(v: float) -> str:
    # 5 dB bins
    edges = [-999, -20, -15, -10, -5, 0, 5, 10, 999]
    labels = ["<-20", "[-20,-15)", "[-15,-10)", "[-10,-5)", "[-5,0)", "[0,5)", "[5,10)", ">=10"]
    for i in range(len(edges)-1):
        if edges[i] <= v < edges[i+1]:
            return labels[i]
    return labels[-1]

def failure_reason(stderr: str) -> str:
    if not stderr:
        return "unknown"
    if "header decode failed" in stderr:
        return "header_fail"
    if "payload decode failed" in stderr:
        return "payload_fail"
    if "invalid payload symbol count" in stderr:
        return "symbol_count"
    return "other"

def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze streaming_compat_results.json grouped failures")
    parser.add_argument('--results', type=str, default=str(RESULTS_JSON), help='Path to streaming_compat_results.json')
    parser.add_argument('--prefix', type=str, default='', help='Only include vectors whose path starts with this prefix')
    parser.add_argument('--out-csv', type=str, default='', help='Optional path to write CSV output')
    parser.add_argument('--out-md', type=str, default='', help='Optional path to write Markdown table output')
    parser.add_argument('--top-n', type=int, default=10, help='Show top N worst bins by failures')
    args = parser.parse_args()

    data = json.loads(Path(args.results).read_text())
    groups: Dict[Tuple[int,int,int,str], Dict[str, Any]] = defaultdict(lambda: {
        'total': 0, 'cpp_ok': 0, 'match': 0,
        'header_fail': 0, 'payload_fail': 0, 'symbol_count': 0, 'other': 0, 'unknown': 0
    })

    for item in data.get('results', []):
        if args.prefix:
            vec = item.get('vector') or ''
            if not vec.startswith(args.prefix):
                continue
        m = item.get('metadata', {})
        sf = int(m.get('sf') or 0)
        bw = int(m.get('bw') or 0)
        cr = int(m.get('cr') or 0)
        snr = float(m.get('snr_db') or 0.0)
        binlab = snr_bin(snr)
        key = (sf, bw, cr, binlab)
        g = groups[key]
        g['total'] += 1
        cpp = item.get('cpp_stream', {})
        if cpp.get('status') == 'success' and (cpp.get('payload_hex') or ''):
            g['cpp_ok'] += 1
        gr = item.get('gnu_radio', {})
        exp = ''
        for fr in gr.get('frames') or []:
            if fr.get('hex'):
                exp = str(fr['hex']).replace(' ', '').lower()
                break
        got = (cpp.get('payload_hex') or '').strip().lower()
        if exp and got and exp == got and cpp.get('status') == 'success':
            g['match'] += 1
        # reason
        if cpp.get('status') != 'success':
            g[failure_reason(cpp.get('stderr') or '')] += 1

    # Print compact summary sorted by SF, BW, CR, SNR bin
    headers = ["sf","bw","cr","snr_bin","total","cpp_ok","match","header_fail","payload_fail","symbol_count","other","unknown"]
    # prepare rows
    rows = []
    for (sf,bw,cr,binlab) in sorted(groups.keys()):
        g = groups[(sf,bw,cr,binlab)]
        rows.append([sf,bw,cr,binlab,g['total'],g['cpp_ok'],g['match'],g['header_fail'],g['payload_fail'],g['symbol_count'],g['other'],g['unknown']])

    # print markdown table to stdout
    print('| ' + ' | '.join(headers) + ' |')
    print('|' + '|'.join(['---'] * len(headers)) + '|')
    for row in rows:
        print('| ' + ' | '.join(map(str,row)) + ' |')

    # optional CSV output
    if args.out_csv:
        import csv
        with open(args.out_csv, 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(headers)
            writer.writerows(rows)

    # optional Markdown output
    if args.out_md:
        with open(args.out_md, 'w') as f:
            f.write('| ' + ' | '.join(headers) + ' |\n')
            f.write('|' + '|'.join(['---'] * len(headers)) + '|\n')
            for row in rows:
                f.write('| ' + ' | '.join(map(str,row)) + ' |\n')

    # Top-N worst bins by (failures = total - cpp_ok), tie-breaker by header_fail desc
    fail_list = []
    for (sf,bw,cr,binlab), g in groups.items():
        failures = g['total'] - g['cpp_ok']
        fail_list.append((failures, g['header_fail'], sf, bw, cr, binlab, g))
    fail_list.sort(reverse=True)
    top_n = fail_list[:max(0, args.top_n)]
    if top_n:
        print('\nTop failure bins:')
        print('| sf | bw | cr | snr_bin | failures | total | header_fail | payload_fail | other |')
        print('|---|---|---|---|---|---|---|---|---|')
        for failures, hdr_fail, sf, bw, cr, binlab, g in top_n:
            print(f"| {sf} | {bw} | {cr} | {binlab} | {failures} | {g['total']} | {g['header_fail']} | {g['payload_fail']} | {g['other']} |")

if __name__ == '__main__':
    main()
