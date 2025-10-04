#!/usr/bin/env python3
"""Generate payload truth table using the python receiver for golden vectors."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Dict, Any, List

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ROOTS = [
    Path('golden_vectors/new_batch'),
    Path('golden_vectors/extended_batch'),
    Path('golden_vectors/extended_batch_crc_off'),
    Path('golden_vectors/extended_batch_impl'),
    Path('golden_vectors/demo'),
    Path('golden_vectors/demo_batch'),
    Path('golden_vectors/custom'),
]


def run_sdr_lora_cli(cf32: Path, fast: bool) -> Dict[str, Any]:
    cmd = ['python', '-m', 'scripts.sdr_lora_cli', 'decode', str(cf32), '-v']
    if fast:
        cmd.append('--fast')
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired as exc:
        return {
            'status': 'timeout',
            'stderr': exc.stderr.decode() if isinstance(exc.stderr, bytes) else (exc.stderr or ''),
        }
    if proc.returncode != 0:
        return {
            'status': 'error',
            'returncode': proc.returncode,
            'stderr': proc.stderr.strip(),
        }
    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        return {
            'status': 'error',
            'returncode': proc.returncode,
            'stderr': f'JSON decode failed: {exc}\nRaw: {proc.stdout[:200]}',
        }
    entry: Dict[str, Any] = {
        'status': 'ok',
        'sf': data.get('sf'),
        'bw': data.get('bw'),
        'fs': data.get('fs'),
        'expected_hex': (data.get('expected') or '').lower(),
        'payload_hex': None,
        'count': 0,
    }
    found = data.get('found') or []
    entry['count'] = len(found)
    if found:
        entry['payload_hex'] = (found[0].get('hex') or '').lower()
        entry['hdr_ok'] = int(found[0].get('hdr_ok', 0))
        entry['crc_ok'] = int(found[0].get('crc_ok', 0))
        entry['cr'] = int(found[0].get('cr', 0))
        entry['ih'] = int(found[0].get('ih', 0))
    return entry


def collect_truth(roots: List[Path], fast: bool) -> Dict[str, Any]:
    truth: Dict[str, Any] = {}
    for root in roots:
        base = (ROOT / root).resolve()
        if not base.exists():
            continue
        for json_path in sorted(base.glob('*.json')):
            cf32_path = json_path.with_suffix('.cf32')
            rel = cf32_path.relative_to(ROOT)
            if not cf32_path.exists():
                truth[str(rel)] = {'status': 'missing_cf32'}
                continue
            truth[str(rel)] = run_sdr_lora_cli(cf32_path, fast=fast)
    return truth


def main():
    parser = argparse.ArgumentParser(description='Generate python payload truth table')
    parser.add_argument('--roots', nargs='*', default=None,
                        help='Root directories (relative to repo) containing vector JSON files')
    parser.add_argument('--output', default='results/python_truth.json',
                        help='Destination JSON path (relative to repo root)')
    parser.add_argument('--no-fast', action='store_true',
                        help='Disable --fast flag when invoking scripts.sdr_lora_cli (slower, but thorough)')
    args = parser.parse_args()

    if args.roots:
        roots = [Path(r) for r in args.roots]
    else:
        roots = DEFAULT_ROOTS

    out_path = (ROOT / args.output).resolve()
    if out_path.exists():
        existing = json.loads(out_path.read_text())
    else:
        existing = {}

    fast = not args.no_fast
    truth = collect_truth(roots, fast=fast)
    existing.update(truth)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(existing, indent=2))
    print(f'Updated {out_path} with {len(truth)} entries (total {len(existing)})')


if __name__ == '__main__':
    main()
