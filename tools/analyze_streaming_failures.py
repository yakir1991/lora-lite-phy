#!/usr/bin/env python3
# This file provides the 'analyze streaming failures' functionality for the LoRa Lite PHY toolkit.
"""Analyze streaming_compat_results.json and print grouped stats.

Groups by (sf, bw, cr) and SNR bins, reporting:
 - total, cpp_success_count, match_count
 - header decode failures vs payload failures (heuristic via stderr)
"""

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) json.
import json
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports the module(s) argparse.
import argparse
# Imports specific objects with 'from collections import defaultdict'.
from collections import defaultdict
# Imports specific objects with 'from typing import Dict, Any, Tuple'.
from typing import Dict, Any, Tuple

# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `RESULTS_JSON = ROOT / 'results' / 'streaming_compat_results.json'`.
RESULTS_JSON = ROOT / 'results' / 'streaming_compat_results.json'

# Defines the function snr_bin.
def snr_bin(v: float) -> str:
    # 5 dB bins
    # Executes the statement `edges = [-999, -20, -15, -10, -5, 0, 5, 10, 999]`.
    edges = [-999, -20, -15, -10, -5, 0, 5, 10, 999]
    # Executes the statement `labels = ["<-20", "[-20,-15)", "[-15,-10)", "[-10,-5)", "[-5,0)", "[0,5)", "[5,10)", ">=10"]`.
    labels = ["<-20", "[-20,-15)", "[-15,-10)", "[-10,-5)", "[-5,0)", "[0,5)", "[5,10)", ">=10"]
    # Starts a loop iterating over a sequence.
    for i in range(len(edges)-1):
        # Begins a conditional branch to check a condition.
        if edges[i] <= v < edges[i+1]:
            # Returns the computed value to the caller.
            return labels[i]
    # Returns the computed value to the caller.
    return labels[-1]

# Defines the function failure_reason.
def failure_reason(stderr: str) -> str:
    # Begins a conditional branch to check a condition.
    if not stderr:
        # Returns the computed value to the caller.
        return "unknown"
    # Begins a conditional branch to check a condition.
    if "header decode failed" in stderr:
        # Returns the computed value to the caller.
        return "header_fail"
    # Begins a conditional branch to check a condition.
    if "payload decode failed" in stderr:
        # Returns the computed value to the caller.
        return "payload_fail"
    # Begins a conditional branch to check a condition.
    if "invalid payload symbol count" in stderr:
        # Returns the computed value to the caller.
        return "symbol_count"
    # Returns the computed value to the caller.
    return "other"

# Defines the function main.
def main() -> None:
    # Executes the statement `parser = argparse.ArgumentParser(description="Analyze streaming_compat_results.json grouped failures")`.
    parser = argparse.ArgumentParser(description="Analyze streaming_compat_results.json grouped failures")
    # Configures the argument parser for the CLI.
    parser.add_argument('--results', type=str, default=str(RESULTS_JSON), help='Path to streaming_compat_results.json')
    # Configures the argument parser for the CLI.
    parser.add_argument('--prefix', type=str, default='', help='Only include vectors whose path starts with this prefix')
    # Configures the argument parser for the CLI.
    parser.add_argument('--out-csv', type=str, default='', help='Optional path to write CSV output')
    # Configures the argument parser for the CLI.
    parser.add_argument('--out-md', type=str, default='', help='Optional path to write Markdown table output')
    # Configures the argument parser for the CLI.
    parser.add_argument('--top-n', type=int, default=10, help='Show top N worst bins by failures')
    # Executes the statement `args = parser.parse_args()`.
    args = parser.parse_args()

    # Executes the statement `data = json.loads(Path(args.results).read_text())`.
    data = json.loads(Path(args.results).read_text())
    # Executes the statement `groups: Dict[Tuple[int,int,int,str], Dict[str, Any]] = defaultdict(lambda: {`.
    groups: Dict[Tuple[int,int,int,str], Dict[str, Any]] = defaultdict(lambda: {
        # Executes the statement `'total': 0, 'cpp_ok': 0, 'match': 0,`.
        'total': 0, 'cpp_ok': 0, 'match': 0,
        # Executes the statement `'header_fail': 0, 'payload_fail': 0, 'symbol_count': 0, 'other': 0, 'unknown': 0`.
        'header_fail': 0, 'payload_fail': 0, 'symbol_count': 0, 'other': 0, 'unknown': 0
    # Executes the statement `})`.
    })

    # Starts a loop iterating over a sequence.
    for item in data.get('results', []):
        # Begins a conditional branch to check a condition.
        if args.prefix:
            # Executes the statement `vec = item.get('vector') or ''`.
            vec = item.get('vector') or ''
            # Begins a conditional branch to check a condition.
            if not vec.startswith(args.prefix):
                # Skips to the next iteration of the loop.
                continue
        # Executes the statement `m = item.get('metadata', {})`.
        m = item.get('metadata', {})
        # Executes the statement `sf = int(m.get('sf') or 0)`.
        sf = int(m.get('sf') or 0)
        # Executes the statement `bw = int(m.get('bw') or 0)`.
        bw = int(m.get('bw') or 0)
        # Executes the statement `cr = int(m.get('cr') or 0)`.
        cr = int(m.get('cr') or 0)
        # Executes the statement `snr = float(m.get('snr_db') or 0.0)`.
        snr = float(m.get('snr_db') or 0.0)
        # Executes the statement `binlab = snr_bin(snr)`.
        binlab = snr_bin(snr)
        # Executes the statement `key = (sf, bw, cr, binlab)`.
        key = (sf, bw, cr, binlab)
        # Executes the statement `g = groups[key]`.
        g = groups[key]
        # Executes the statement `g['total'] += 1`.
        g['total'] += 1
        # Executes the statement `cpp = item.get('cpp_stream', {})`.
        cpp = item.get('cpp_stream', {})
        # Begins a conditional branch to check a condition.
        if cpp.get('status') == 'success' and (cpp.get('payload_hex') or ''):
            # Executes the statement `g['cpp_ok'] += 1`.
            g['cpp_ok'] += 1
        # Executes the statement `gr = item.get('gnu_radio', {})`.
        gr = item.get('gnu_radio', {})
        # Executes the statement `exp = ''`.
        exp = ''
        # Starts a loop iterating over a sequence.
        for fr in gr.get('frames') or []:
            # Begins a conditional branch to check a condition.
            if fr.get('hex'):
                # Executes the statement `exp = str(fr['hex']).replace(' ', '').lower()`.
                exp = str(fr['hex']).replace(' ', '').lower()
                # Exits the nearest enclosing loop early.
                break
        # Executes the statement `got = (cpp.get('payload_hex') or '').strip().lower()`.
        got = (cpp.get('payload_hex') or '').strip().lower()
        # Begins a conditional branch to check a condition.
        if exp and got and exp == got and cpp.get('status') == 'success':
            # Executes the statement `g['match'] += 1`.
            g['match'] += 1
        # reason
        # Begins a conditional branch to check a condition.
        if cpp.get('status') != 'success':
            # Executes the statement `g[failure_reason(cpp.get('stderr') or '')] += 1`.
            g[failure_reason(cpp.get('stderr') or '')] += 1

    # Print compact summary sorted by SF, BW, CR, SNR bin
    # Executes the statement `headers = ["sf","bw","cr","snr_bin","total","cpp_ok","match","header_fail","payload_fail","symbol_count","other","unknown"]`.
    headers = ["sf","bw","cr","snr_bin","total","cpp_ok","match","header_fail","payload_fail","symbol_count","other","unknown"]
    # prepare rows
    # Executes the statement `rows = []`.
    rows = []
    # Starts a loop iterating over a sequence.
    for (sf,bw,cr,binlab) in sorted(groups.keys()):
        # Executes the statement `g = groups[(sf,bw,cr,binlab)]`.
        g = groups[(sf,bw,cr,binlab)]
        # Executes the statement `rows.append([sf,bw,cr,binlab,g['total'],g['cpp_ok'],g['match'],g['header_fail'],g['payload_fail'],g['symbol_count'],g['other'],g['unknown']])`.
        rows.append([sf,bw,cr,binlab,g['total'],g['cpp_ok'],g['match'],g['header_fail'],g['payload_fail'],g['symbol_count'],g['other'],g['unknown']])

    # print markdown table to stdout
    # Outputs diagnostic or user-facing text.
    print('| ' + ' | '.join(headers) + ' |')
    # Outputs diagnostic or user-facing text.
    print('|' + '|'.join(['---'] * len(headers)) + '|')
    # Starts a loop iterating over a sequence.
    for row in rows:
        # Outputs diagnostic or user-facing text.
        print('| ' + ' | '.join(map(str,row)) + ' |')

    # optional CSV output
    # Begins a conditional branch to check a condition.
    if args.out_csv:
        # Imports the module(s) csv.
        import csv
        # Opens a context manager scope for managed resources.
        with open(args.out_csv, 'w', newline='') as f:
            # Executes the statement `writer = csv.writer(f)`.
            writer = csv.writer(f)
            # Executes the statement `writer.writerow(headers)`.
            writer.writerow(headers)
            # Executes the statement `writer.writerows(rows)`.
            writer.writerows(rows)

    # optional Markdown output
    # Begins a conditional branch to check a condition.
    if args.out_md:
        # Opens a context manager scope for managed resources.
        with open(args.out_md, 'w') as f:
            # Executes the statement `f.write('| ' + ' | '.join(headers) + ' |\n')`.
            f.write('| ' + ' | '.join(headers) + ' |\n')
            # Executes the statement `f.write('|' + '|'.join(['---'] * len(headers)) + '|\n')`.
            f.write('|' + '|'.join(['---'] * len(headers)) + '|\n')
            # Starts a loop iterating over a sequence.
            for row in rows:
                # Executes the statement `f.write('| ' + ' | '.join(map(str,row)) + ' |\n')`.
                f.write('| ' + ' | '.join(map(str,row)) + ' |\n')

    # Top-N worst bins by (failures = total - cpp_ok), tie-breaker by header_fail desc
    # Executes the statement `fail_list = []`.
    fail_list = []
    # Starts a loop iterating over a sequence.
    for (sf,bw,cr,binlab), g in groups.items():
        # Executes the statement `failures = g['total'] - g['cpp_ok']`.
        failures = g['total'] - g['cpp_ok']
        # Executes the statement `fail_list.append((failures, g['header_fail'], sf, bw, cr, binlab, g))`.
        fail_list.append((failures, g['header_fail'], sf, bw, cr, binlab, g))
    # Executes the statement `fail_list.sort(reverse=True)`.
    fail_list.sort(reverse=True)
    # Executes the statement `top_n = fail_list[:max(0, args.top_n)]`.
    top_n = fail_list[:max(0, args.top_n)]
    # Begins a conditional branch to check a condition.
    if top_n:
        # Outputs diagnostic or user-facing text.
        print('\nTop failure bins:')
        # Outputs diagnostic or user-facing text.
        print('| sf | bw | cr | snr_bin | failures | total | header_fail | payload_fail | other |')
        # Outputs diagnostic or user-facing text.
        print('|---|---|---|---|---|---|---|---|---|')
        # Starts a loop iterating over a sequence.
        for failures, hdr_fail, sf, bw, cr, binlab, g in top_n:
            # Outputs diagnostic or user-facing text.
            print(f"| {sf} | {bw} | {cr} | {binlab} | {failures} | {g['total']} | {g['header_fail']} | {g['payload_fail']} | {g['other']} |")

# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
