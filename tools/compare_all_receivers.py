#!/usr/bin/env python3
"""Compare payload outputs of C++, Python, and GNU Radio receivers."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from subprocess import run
from typing import Dict, Any, Tuple

ROOT = Path(__file__).resolve().parents[1]
PY_TRUTH = ROOT / 'results/python_truth.json'
GNUR_TRUTH = ROOT / 'results/gnur_truth.json'


def ldro_flag(meta: Dict[str, Any]) -> str:
    mode = int(meta.get('ldro_mode', 0) or 0)
    sf = int(meta.get('sf', 7))
    if mode == 1:
        return '1'
    if mode == 2 and sf >= 11:
        return '1'
    return '0'


def run_cpp(cf32: Path, meta: Dict[str, Any]) -> Tuple[bool, str, str]:
    cmd = [
        str(ROOT / 'cpp_receiver' / 'build' / 'decode_cli'),
        '--sf', str(meta.get('sf')),
        '--bw', str(meta.get('bw')),
        '--fs', str(meta.get('samp_rate', meta.get('fs', 0))),
        '--ldro', ldro_flag(meta),
    ]
    if meta.get('impl_header'):
        cmd += [
            '--implicit-header',
            '--impl-len', str(meta.get('payload_len', 0)),
            '--impl-cr', str(meta.get('cr', 1)),
            '--impl-crc', '1' if meta.get('crc', True) else '0',
        ]
    cmd.append(str(cf32))
    proc = run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        return False, '', proc.stderr
    hexval = ''
    for line in proc.stdout.splitlines():
        if line.startswith('payload_hex='):
            hexval = line.split('=', 1)[1].strip().lower()
            break
    return True, hexval, ''


def load_meta(cf32: Path) -> Dict[str, Any]:
    meta_path = cf32.with_suffix('.json')
    return json.loads(meta_path.read_text())


def categorize(py_info: Dict[str, Any], gnur_info: Dict[str, Any], cpp_ok: bool, cpp_hex: str) -> str:
    py_hex = (py_info.get('payload_hex') or '').lower()
    gnur_payloads = [p.lower() for p in gnur_info.get('payloads', [])] if gnur_info.get('status') == 'ok' else []

    py_ok = bool(py_hex)
    gnur_ok = bool(gnur_payloads)
    cpp_match_py = cpp_hex == py_hex if (py_ok and cpp_ok) else False
    cpp_match_gnur = cpp_hex in gnur_payloads if (gnur_ok and cpp_ok) else False

    if py_ok and cpp_ok and gnur_ok and cpp_match_py and cpp_match_gnur:
        return 'all_match'
    if py_ok and cpp_ok and cpp_match_py and not gnur_ok:
        return 'cpp_python_only'
    if cpp_ok and gnur_ok and cpp_match_gnur and not py_ok:
        return 'cpp_gnur_only'
    if py_ok and gnur_ok and py_hex in gnur_payloads and not cpp_ok:
        return 'python_gnur_only'
    if cpp_ok and py_ok and not cpp_match_py:
        return 'cpp_vs_python_mismatch'
    if cpp_ok and gnur_ok and not cpp_match_gnur:
        return 'cpp_vs_gnur_mismatch'
    if py_ok and gnur_ok and py_hex not in gnur_payloads:
        return 'python_vs_gnur_mismatch'
    if cpp_ok and not (py_ok or gnur_ok):
        return 'cpp_only'
    if py_ok and not (cpp_ok or gnur_ok):
        return 'python_only'
    if gnur_ok and not (cpp_ok or py_ok):
        return 'gnur_only'
    return 'all_fail'


def main():
    parser = argparse.ArgumentParser(description='Compare outputs from all receivers')
    parser.add_argument('--output', default='results/receiver_comparison.json', help='Output summary JSON path')
    parser.add_argument('--limit', type=int, help='Limit number of vectors for quick tests')
    args = parser.parse_args()

    python_truth = json.loads(PY_TRUTH.read_text()) if PY_TRUTH.exists() else {}
    gnur_truth = json.loads(GNUR_TRUTH.read_text()) if GNUR_TRUTH.exists() else {}

    vectors = sorted(python_truth.keys() | gnur_truth.keys())
    if args.limit:
        vectors = vectors[:args.limit]

    summary: Dict[str, Any] = {}
    counts: Dict[str, int] = {}

    for rel in vectors:
        cf32 = (ROOT / rel).resolve()
        meta = load_meta(cf32)
        py_info = python_truth.get(rel, {'status': 'missing'})
        gnur_info = gnur_truth.get(rel, {'status': 'missing'})

        cpp_ok, cpp_hex, cpp_err = run_cpp(cf32, meta)
        category = categorize(py_info, gnur_info, cpp_ok, cpp_hex)
        counts[category] = counts.get(category, 0) + 1

        entry = {
            'sf': meta.get('sf'),
            'cr': meta.get('cr'),
            'ldro_mode': meta.get('ldro_mode'),
            'impl_header': bool(meta.get('impl_header', False)),
            'python': py_info,
            'gnuradio': gnur_info,
            'cpp_ok': cpp_ok,
            'cpp_payload_hex': cpp_hex,
            'cpp_error': cpp_err if not cpp_ok else None,
            'category': category,
        }
        summary[rel] = entry

    out_path = (ROOT / args.output).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({'counts': counts, 'entries': summary}, indent=2))
    print(f'Wrote summary to {out_path}')
    print(json.dumps(counts, indent=2))


if __name__ == '__main__':
    main()
