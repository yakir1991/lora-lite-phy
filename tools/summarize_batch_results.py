#!/usr/bin/env python3
"""Summarize compare_batch_compat results into CSV and Markdown tables.

Reads results/batch_compat_results.json and writes:
  - results/batch_compat_summary.csv
  - results/batch_compat_summary.md

Columns: vector, sf, bw, fs, cr, ldro_mode, crc, impl_header, snr_db, cpp_status, payload_len, match
"""

from __future__ import annotations

import csv
import json
import re
from pathlib import Path
from typing import Any, Dict, List

ROOT = Path(__file__).resolve().parents[1]
RESULTS_JSON = ROOT / 'results' / 'batch_compat_results.json'
CSV_OUT = ROOT / 'results' / 'batch_compat_summary.csv'
MD_OUT = ROOT / 'results' / 'batch_compat_summary.md'


def parse_payload_len(stdout: str) -> int:
    for line in stdout.splitlines():
        m = re.search(r"payload_len=(\d+)", line)
        if m:
            return int(m.group(1))
    return 0


def extract_expected_hex(gr_entry: Dict[str, Any]) -> str:
    frames = gr_entry.get('frames') or []
    for fr in frames:
        hx = fr.get('hex')
        if hx:
            return str(hx).replace(' ', '').lower()
    return ''


def main() -> None:
    data = json.loads(RESULTS_JSON.read_text())
    rows: List[Dict[str, Any]] = []
    for item in data.get('results', []):
        meta = item.get('metadata', {})
        cpp = item.get('cpp_batch', {})
        gr = item.get('gnu_radio', {})
        expected = extract_expected_hex(gr)
        got = (cpp.get('payload_hex') or '').strip().lower()
        match = bool(expected and got and (expected == got) and cpp.get('status') == 'success')
        row = {
            'vector': Path(item.get('vector', '')).name,
            'sf': meta.get('sf'),
            'bw': meta.get('bw'),
            'fs': meta.get('samp_rate') or meta.get('sample_rate'),
            'cr': meta.get('cr'),
            'ldro_mode': meta.get('ldro_mode'),
            'crc': meta.get('crc'),
            'impl_header': bool(meta.get('impl_header') or meta.get('implicit_header')),
            'snr_db': meta.get('snr_db'),
            'cpp_status': cpp.get('status'),
            'payload_len': parse_payload_len(cpp.get('stdout', '') or ''),
            'match': match,
        }
        rows.append(row)

    # Sort by sf asc, bw asc, cr asc, then SNR (worst first)
    def sort_key(r: Dict[str, Any]):
        return (
            int(r.get('sf') or 0),
            int(r.get('bw') or 0),
            int(r.get('cr') or 0),
            float(r.get('snr_db') or 0.0),
        )

    rows.sort(key=sort_key)

    # Write CSV
    fieldnames = ['vector', 'sf', 'bw', 'fs', 'cr', 'ldro_mode', 'crc', 'impl_header', 'snr_db', 'cpp_status', 'payload_len', 'match']
    CSV_OUT.parent.mkdir(parents=True, exist_ok=True)
    with CSV_OUT.open('w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for r in rows:
            w.writerow(r)

    # Write Markdown table (first 100 rows to keep it readable)
    lines: List[str] = []
    lines.append('| ' + ' | '.join(fieldnames) + ' |')
    lines.append('|' + '|'.join(['---'] * len(fieldnames)) + '|')
    for r in rows[:100]:
        vals = [str(r.get(k, '')) for k in fieldnames]
        lines.append('| ' + ' | '.join(vals) + ' |')
    MD_OUT.write_text('\n'.join(lines))

    print(f"Wrote {CSV_OUT} and {MD_OUT}")


if __name__ == '__main__':
    main()
