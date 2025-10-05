#!/usr/bin/env python3
"""Robust cross-receiver comparison with per-vector timeouts and fallback.

- Loads Python and GNU Radio truth tables (from results/python_truth.json and
  results/gnur_truth.json) if available.
- Collects .cf32 vectors from the specified roots and their sidecar metadata (.json).
- Runs the C++ receiver with proper flags (implicit header, CRC, LDRO, sync-word).
- Optional fallback: re-run C++ with --skip-syncword when the first attempt fails.
- Writes a summary JSON and prints category counts.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from subprocess import run, TimeoutExpired
from typing import Dict, Any, List, Tuple


ROOT = Path(__file__).resolve().parents[1]
PY_TRUTH = ROOT / 'results' / 'python_truth.json'
GNUR_TRUTH = ROOT / 'results' / 'gnur_truth.json'

DEFAULT_ROOTS = [
    Path('golden_vectors/new_batch'),
    Path('golden_vectors/extended_batch'),
    Path('golden_vectors/extended_batch_crc_off'),
    Path('golden_vectors/extended_batch_impl'),
    Path('golden_vectors/demo_batch'),
    Path('golden_vectors/custom'),
]


def load_truth(path: Path) -> Dict[str, Any]:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text())
    except Exception:
        return {}


def load_meta(cf32: Path) -> Dict[str, Any]:
    meta_path = cf32.with_suffix('.json')
    return json.loads(meta_path.read_text())


def ldro_flag(meta: Dict[str, Any]) -> str:
    mode = int(meta.get('ldro_mode', 0) or 0)
    sf = int(meta.get('sf', 7))
    if mode == 1:
        return '1'
    if mode == 2 and sf >= 11:
        return '1'
    return '0'


def make_cpp_cmd(cf32: Path, meta: Dict[str, Any], skip_syncword: bool = False) -> List[str]:
    cmd = [
        str(ROOT / 'cpp_receiver' / 'build' / 'decode_cli'),
        '--sf', str(meta.get('sf')),
        '--bw', str(meta.get('bw')),
        '--fs', str(meta.get('samp_rate', meta.get('fs', 0))),
        '--ldro', ldro_flag(meta),
    ]
    if 'sync_word' in meta:
        try:
            cmd += ['--sync-word', hex(int(meta['sync_word']))]
        except Exception:
            pass
    if meta.get('impl_header'):
        cmd += [
            '--implicit-header',
            '--payload-len', str(meta.get('payload_len', 0)),
            '--cr', str(meta.get('cr', 1)),
            '--has-crc' if meta.get('crc', True) else '--no-crc',
        ]
    if skip_syncword:
        cmd.append('--skip-syncword')
    cmd.append(str(cf32))
    return cmd


def run_cpp_with_timeout(cf32: Path, meta: Dict[str, Any], timeout_s: int, fallback_skip: bool) -> Tuple[bool, str, Dict[str, Any]]:
    """Run C++ decoder; optionally retry with --skip-syncword; return (ok, hex, diag)."""
    def _run(cmd: List[str]) -> Tuple[bool, str, Dict[str, Any]]:
        try:
            proc = run(cmd, capture_output=True, text=True, timeout=timeout_s)
        except TimeoutExpired:
            return False, '', {'status': 'timeout', 'timeout_sec': timeout_s}
        if proc.returncode != 0:
            return False, '', {
                'status': 'error', 'ret': proc.returncode,
                'stderr': proc.stderr[-400:], 'stdout': proc.stdout[-400:],
            }
        hexval = ''
        for line in proc.stdout.splitlines():
            if line.startswith('payload_hex='):
                hexval = line.split('=', 1)[1].strip().lower()
                break
        return True, hexval, {'status': 'ok'}

    ok, hexval, diag = _run(make_cpp_cmd(cf32, meta, skip_syncword=False))
    if ok:
        return ok, hexval, diag
    if fallback_skip:
        ok2, hex2, diag2 = _run(make_cpp_cmd(cf32, meta, skip_syncword=True))
        if ok2:
            return ok2, hex2, {'status': 'ok', 'used_skip_syncword': True}
        return ok2, hex2, {'first': diag, 'second': diag2}
    return ok, hexval, diag


def collect_vectors(roots: List[Path]) -> List[Path]:
    vecs: List[Path] = []
    for r in roots:
        base = (ROOT / r).resolve()
        if not base.exists():
            continue
        vecs.extend(sorted(base.glob('*.cf32')))
    return vecs


def categorize(py_info: Dict[str, Any] | None, gnur_info: Dict[str, Any] | None, cpp_ok: bool, cpp_hex: str) -> str:
    py_hex = ((py_info or {}).get('payload_hex') or '').lower()
    gnur_payloads = [p.lower() for p in ((gnur_info or {}).get('payloads') or [])] if (gnur_info or {}).get('status') == 'ok' else []

    py_ok = bool(py_hex)
    gnur_ok = bool(gnur_payloads)
    cpp_match_py = (cpp_hex == py_hex) if (py_ok and cpp_ok) else False
    cpp_match_gnur = (cpp_hex in gnur_payloads) if (gnur_ok and cpp_ok) else False

    if py_ok and cpp_ok and gnur_ok and cpp_match_py and cpp_match_gnur:
        return 'all_match'
    if py_ok and cpp_ok and cpp_match_py and not gnur_ok:
        return 'cpp_python_only'
    if cpp_ok and gnur_ok and cpp_match_gnur and not py_ok:
        return 'cpp_gnur_only'
    if py_ok and gnur_ok and (py_hex in gnur_payloads) and not cpp_ok:
        return 'python_gnur_only'
    if cpp_ok and py_ok and not cpp_match_py:
        return 'cpp_vs_python_mismatch'
    if cpp_ok and gnur_ok and not cpp_match_gnur:
        return 'cpp_vs_gnur_mismatch'
    if py_ok and gnur_ok and (py_hex not in gnur_payloads):
        return 'python_vs_gnur_mismatch'
    if cpp_ok and not (py_ok or gnur_ok):
        return 'cpp_only'
    if py_ok and not (cpp_ok or gnur_ok):
        return 'python_only'
    if gnur_ok and not (cpp_ok or py_ok):
        return 'gnur_only'
    return 'all_fail'


def main() -> None:
    ap = argparse.ArgumentParser(description='Robust cross-receiver comparison (v2)')
    ap.add_argument('--roots', nargs='*', default=None, help='Vector roots (relative to repo root)')
    ap.add_argument('--limit', type=int, default=None, help='Limit number of vectors')
    ap.add_argument('--timeout', type=int, default=45, help='Per-vector timeout (seconds) for C++')
    ap.add_argument('--fallback-skip', action='store_true', help='Fallback to --skip-syncword on C++ failure')
    ap.add_argument('--output', default='results/receiver_comparison_v2.json', help='Output JSON path')
    ap.add_argument('--progress-every', type=int, default=1, help='Print progress every N vectors (default 1)')
    args = ap.parse_args()

    py_truth = load_truth(PY_TRUTH)
    gnur_truth = load_truth(GNUR_TRUTH)

    roots = [Path(r) for r in args.roots] if args.roots else DEFAULT_ROOTS
    vectors = collect_vectors(roots)
    if args.limit:
        vectors = vectors[: args.limit]

    counts: Dict[str, int] = {}
    entries: Dict[str, Any] = {}

    for idx, cf32 in enumerate(vectors, start=1):
        rel = str(cf32.relative_to(ROOT))
        try:
            meta = load_meta(cf32)
        except Exception as e:
            entries[rel] = {'status': 'meta_error', 'error': repr(e)}
            counts['meta_error'] = counts.get('meta_error', 0) + 1
            continue

        cpp_ok, cpp_hex, diag = run_cpp_with_timeout(cf32, meta, timeout_s=args.timeout, fallback_skip=args.fallback_skip)
        py_info = py_truth.get(rel)
        gnur_info = gnur_truth.get(rel)
        cat = categorize(py_info, gnur_info, cpp_ok, cpp_hex)
        counts[cat] = counts.get(cat, 0) + 1

        entries[rel] = {
            'sf': meta.get('sf'),
            'cr': meta.get('cr'),
            'ldro_mode': meta.get('ldro_mode'),
            'impl_header': bool(meta.get('impl_header', False)),
            'python': py_info or {'status': 'missing'},
            'gnuradio': gnur_info or {'status': 'missing'},
            'cpp_ok': cpp_ok,
            'cpp_payload_hex': cpp_hex,
            'cpp_diag': diag,
            'category': cat,
        }

        if (args.progress_every and (idx % args.progress_every == 0)) or idx == len(vectors):
            print(f"[{idx}/{len(vectors)}] {cf32.name} -> {cat}")
            sys.stdout.flush()

    out_path = (ROOT / args.output).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({'counts': counts, 'entries': entries}, indent=2))
    print(json.dumps(counts, indent=2))


if __name__ == '__main__':
    main()


