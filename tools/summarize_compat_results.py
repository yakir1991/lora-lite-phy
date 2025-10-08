#!/usr/bin/env python3
# This file provides the 'summarize compat results' functionality for the LoRa Lite PHY toolkit.
"""Summarize compare_streaming_compat results into CSV and Markdown tables.

Reads results/streaming_compat_results.json and writes:
  - results/streaming_compat_summary.csv
  - results/streaming_compat_summary.md

Columns: vector, sf, bw, fs, cr, ldro_mode, crc, impl_header, snr_db, cpp_status, payload_len, match
"""

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) csv.
import csv
# Imports the module(s) json.
import json
# Imports the module(s) re.
import re
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from typing import Any, Dict, List'.
from typing import Any, Dict, List

# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `RESULTS_JSON = ROOT / 'results' / 'streaming_compat_results.json'`.
RESULTS_JSON = ROOT / 'results' / 'streaming_compat_results.json'
# Executes the statement `CSV_OUT = ROOT / 'results' / 'streaming_compat_summary.csv'`.
CSV_OUT = ROOT / 'results' / 'streaming_compat_summary.csv'
# Executes the statement `MD_OUT = ROOT / 'results' / 'streaming_compat_summary.md'`.
MD_OUT = ROOT / 'results' / 'streaming_compat_summary.md'


# Defines the function parse_payload_len.
def parse_payload_len(stdout: str) -> int:
    # Starts a loop iterating over a sequence.
    for line in stdout.splitlines():
        # Executes the statement `m = re.search(r"payload_len=(\d+)", line)`.
        m = re.search(r"payload_len=(\d+)", line)
        # Begins a conditional branch to check a condition.
        if m:
            # Returns the computed value to the caller.
            return int(m.group(1))
    # Returns the computed value to the caller.
    return 0


# Defines the function extract_expected_hex.
def extract_expected_hex(gr_entry: Dict[str, Any]) -> str:
    # Executes the statement `frames = gr_entry.get('frames') or []`.
    frames = gr_entry.get('frames') or []
    # Starts a loop iterating over a sequence.
    for fr in frames:
        # Executes the statement `hx = fr.get('hex')`.
        hx = fr.get('hex')
        # Begins a conditional branch to check a condition.
        if hx:
            # Returns the computed value to the caller.
            return str(hx).replace(' ', '').lower()
    # Returns the computed value to the caller.
    return ''


# Defines the function main.
def main() -> None:
    # Executes the statement `data = json.loads(RESULTS_JSON.read_text())`.
    data = json.loads(RESULTS_JSON.read_text())
    # Executes the statement `rows: List[Dict[str, Any]] = []`.
    rows: List[Dict[str, Any]] = []
    # Starts a loop iterating over a sequence.
    for item in data.get('results', []):
        # Executes the statement `meta = item.get('metadata', {})`.
        meta = item.get('metadata', {})
        # Executes the statement `cpp = item.get('cpp_stream', {})`.
        cpp = item.get('cpp_stream', {})
        # Executes the statement `gr = item.get('gnu_radio', {})`.
        gr = item.get('gnu_radio', {})
        # Executes the statement `expected = extract_expected_hex(gr)`.
        expected = extract_expected_hex(gr)
        # Executes the statement `got = (cpp.get('payload_hex') or '').strip().lower()`.
        got = (cpp.get('payload_hex') or '').strip().lower()
        # Executes the statement `match = bool(expected and got and (expected == got) and cpp.get('status') == 'success')`.
        match = bool(expected and got and (expected == got) and cpp.get('status') == 'success')
        # Executes the statement `row = {`.
        row = {
            # Executes the statement `'vector': Path(item.get('vector', '')).name,`.
            'vector': Path(item.get('vector', '')).name,
            # Executes the statement `'sf': meta.get('sf'),`.
            'sf': meta.get('sf'),
            # Executes the statement `'bw': meta.get('bw'),`.
            'bw': meta.get('bw'),
            # Executes the statement `'fs': meta.get('samp_rate') or meta.get('sample_rate'),`.
            'fs': meta.get('samp_rate') or meta.get('sample_rate'),
            # Executes the statement `'cr': meta.get('cr'),`.
            'cr': meta.get('cr'),
            # Executes the statement `'ldro_mode': meta.get('ldro_mode'),`.
            'ldro_mode': meta.get('ldro_mode'),
            # Executes the statement `'crc': meta.get('crc'),`.
            'crc': meta.get('crc'),
            # Executes the statement `'impl_header': bool(meta.get('impl_header') or meta.get('implicit_header')),`.
            'impl_header': bool(meta.get('impl_header') or meta.get('implicit_header')),
            # Executes the statement `'snr_db': meta.get('snr_db'),`.
            'snr_db': meta.get('snr_db'),
            # Executes the statement `'cpp_status': cpp.get('status'),`.
            'cpp_status': cpp.get('status'),
            # Executes the statement `'payload_len': parse_payload_len(cpp.get('stdout', '') or ''),`.
            'payload_len': parse_payload_len(cpp.get('stdout', '') or ''),
            # Executes the statement `'match': match,`.
            'match': match,
        # Closes the previously opened dictionary or set literal.
        }
        # Executes the statement `rows.append(row)`.
        rows.append(row)

    # Sort by sf asc, bw asc, cr asc, then SNR (worst first)
    # Defines the function sort_key.
    def sort_key(r: Dict[str, Any]):
        # Returns the computed value to the caller.
        return (
            # Executes the statement `int(r.get('sf') or 0),`.
            int(r.get('sf') or 0),
            # Executes the statement `int(r.get('bw') or 0),`.
            int(r.get('bw') or 0),
            # Executes the statement `int(r.get('cr') or 0),`.
            int(r.get('cr') or 0),
            # Executes the statement `float(r.get('snr_db') or 0.0),`.
            float(r.get('snr_db') or 0.0),
        # Closes the previously opened parenthesis grouping.
        )

    # Executes the statement `rows.sort(key=sort_key)`.
    rows.sort(key=sort_key)

    # Write CSV
    # Executes the statement `fieldnames = ['vector', 'sf', 'bw', 'fs', 'cr', 'ldro_mode', 'crc', 'impl_header', 'snr_db', 'cpp_status', 'payload_len', 'match']`.
    fieldnames = ['vector', 'sf', 'bw', 'fs', 'cr', 'ldro_mode', 'crc', 'impl_header', 'snr_db', 'cpp_status', 'payload_len', 'match']
    # Executes the statement `CSV_OUT.parent.mkdir(parents=True, exist_ok=True)`.
    CSV_OUT.parent.mkdir(parents=True, exist_ok=True)
    # Opens a context manager scope for managed resources.
    with CSV_OUT.open('w', newline='') as f:
        # Executes the statement `w = csv.DictWriter(f, fieldnames=fieldnames)`.
        w = csv.DictWriter(f, fieldnames=fieldnames)
        # Executes the statement `w.writeheader()`.
        w.writeheader()
        # Starts a loop iterating over a sequence.
        for r in rows:
            # Executes the statement `w.writerow(r)`.
            w.writerow(r)

    # Write Markdown table (first 100 rows to keep it readable)
    # Executes the statement `lines: List[str] = []`.
    lines: List[str] = []
    # Executes the statement `lines.append('| ' + ' | '.join(fieldnames) + ' |')`.
    lines.append('| ' + ' | '.join(fieldnames) + ' |')
    # Executes the statement `lines.append('|' + '|'.join(['---'] * len(fieldnames)) + '|')`.
    lines.append('|' + '|'.join(['---'] * len(fieldnames)) + '|')
    # Starts a loop iterating over a sequence.
    for r in rows[:100]:
        # Executes the statement `vals = [str(r.get(k, '')) for k in fieldnames]`.
        vals = [str(r.get(k, '')) for k in fieldnames]
        # Executes the statement `lines.append('| ' + ' | '.join(vals) + ' |')`.
        lines.append('| ' + ' | '.join(vals) + ' |')
    # Executes the statement `MD_OUT.write_text('\n'.join(lines))`.
    MD_OUT.write_text('\n'.join(lines))

    # Outputs diagnostic or user-facing text.
    print(f"Wrote {CSV_OUT} and {MD_OUT}")


# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
