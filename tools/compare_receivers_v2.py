#!/usr/bin/env python3
# This file provides the 'compare receivers v2' functionality for the LoRa Lite PHY toolkit.
"""Robust cross-receiver comparison with per-vector timeouts and fallback.

- Loads Python and GNU Radio truth tables (from results/python_truth.json and
  results/gnur_truth.json) if available.
- Collects .cf32 vectors from the specified roots and their sidecar metadata (.json).
- Runs the C++ receiver with proper flags (implicit header, CRC, LDRO, sync-word).
- Optional fallback: re-run C++ with --skip-syncword when the first attempt fails.
- Writes a summary JSON and prints category counts.
"""

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports the module(s) sys.
import sys
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from subprocess import run, TimeoutExpired'.
from subprocess import run, TimeoutExpired
# Imports specific objects with 'from typing import Dict, Any, List, Tuple'.
from typing import Dict, Any, List, Tuple


# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `PY_TRUTH = ROOT / 'results' / 'python_truth.json'`.
PY_TRUTH = ROOT / 'results' / 'python_truth.json'
# Executes the statement `GNUR_TRUTH = ROOT / 'results' / 'gnur_truth.json'`.
GNUR_TRUTH = ROOT / 'results' / 'gnur_truth.json'

# Executes the statement `DEFAULT_ROOTS = [`.
DEFAULT_ROOTS = [
    # Executes the statement `Path('golden_vectors/new_batch'),`.
    Path('golden_vectors/new_batch'),
    # Executes the statement `Path('golden_vectors/extended_batch'),`.
    Path('golden_vectors/extended_batch'),
    # Executes the statement `Path('golden_vectors/extended_batch_crc_off'),`.
    Path('golden_vectors/extended_batch_crc_off'),
    # Executes the statement `Path('golden_vectors/extended_batch_impl'),`.
    Path('golden_vectors/extended_batch_impl'),
    # Executes the statement `Path('golden_vectors/demo_batch'),`.
    Path('golden_vectors/demo_batch'),
    # Executes the statement `Path('golden_vectors/custom'),`.
    Path('golden_vectors/custom'),
# Closes the previously opened list indexing or literal.
]


# Defines the function load_truth.
def load_truth(path: Path) -> Dict[str, Any]:
    # Begins a conditional branch to check a condition.
    if not path.exists():
        # Returns the computed value to the caller.
        return {}
    # Begins a block that monitors for exceptions.
    try:
        # Returns the computed value to the caller.
        return json.loads(path.read_text())
    # Handles a specific exception from the try block.
    except Exception:
        # Returns the computed value to the caller.
        return {}


# Defines the function load_meta.
def load_meta(cf32: Path) -> Dict[str, Any]:
    # Executes the statement `meta_path = cf32.with_suffix('.json')`.
    meta_path = cf32.with_suffix('.json')
    # Returns the computed value to the caller.
    return json.loads(meta_path.read_text())


# Defines the function ldro_flag.
def ldro_flag(meta: Dict[str, Any]) -> str:
    # Executes the statement `mode = int(meta.get('ldro_mode', 0) or 0)`.
    mode = int(meta.get('ldro_mode', 0) or 0)
    # Executes the statement `sf = int(meta.get('sf', 7))`.
    sf = int(meta.get('sf', 7))
    # Begins a conditional branch to check a condition.
    if mode == 1:
        # Returns the computed value to the caller.
        return '1'
    # Begins a conditional branch to check a condition.
    if mode == 2 and sf >= 11:
        # Returns the computed value to the caller.
        return '1'
    # Returns the computed value to the caller.
    return '0'


# Defines the function make_cpp_cmd.
def make_cpp_cmd(cf32: Path, meta: Dict[str, Any], skip_syncword: bool = False) -> List[str]:
    # Executes the statement `cmd = [`.
    cmd = [
        # Executes the statement `str(ROOT / 'cpp_receiver' / 'build' / 'decode_cli'),`.
        str(ROOT / 'cpp_receiver' / 'build' / 'decode_cli'),
        # Executes the statement `'--sf', str(meta.get('sf')),`.
        '--sf', str(meta.get('sf')),
        # Executes the statement `'--bw', str(meta.get('bw')),`.
        '--bw', str(meta.get('bw')),
        # Executes the statement `'--fs', str(meta.get('samp_rate', meta.get('fs', 0))),`.
        '--fs', str(meta.get('samp_rate', meta.get('fs', 0))),
        # Executes the statement `'--ldro', ldro_flag(meta),`.
        '--ldro', ldro_flag(meta),
    # Closes the previously opened list indexing or literal.
    ]
    # Begins a conditional branch to check a condition.
    if 'sync_word' in meta:
        # Begins a block that monitors for exceptions.
        try:
            # Executes the statement `cmd += ['--sync-word', hex(int(meta['sync_word']))]`.
            cmd += ['--sync-word', hex(int(meta['sync_word']))]
        # Handles a specific exception from the try block.
        except Exception:
            # Acts as a no-operation placeholder statement.
            pass
    # Begins a conditional branch to check a condition.
    if meta.get('impl_header'):
        # Executes the statement `cmd += [`.
        cmd += [
            # Executes the statement `'--implicit-header',`.
            '--implicit-header',
            # Executes the statement `'--payload-len', str(meta.get('payload_len', 0)),`.
            '--payload-len', str(meta.get('payload_len', 0)),
            # Executes the statement `'--cr', str(meta.get('cr', 1)),`.
            '--cr', str(meta.get('cr', 1)),
            # Executes the statement `'--has-crc' if meta.get('crc', True) else '--no-crc',`.
            '--has-crc' if meta.get('crc', True) else '--no-crc',
        # Closes the previously opened list indexing or literal.
        ]
    # Begins a conditional branch to check a condition.
    if skip_syncword:
        # Executes the statement `cmd.append('--skip-syncword')`.
        cmd.append('--skip-syncword')
    # Executes the statement `cmd.append(str(cf32))`.
    cmd.append(str(cf32))
    # Returns the computed value to the caller.
    return cmd


# Defines the function run_cpp_with_timeout.
def run_cpp_with_timeout(cf32: Path, meta: Dict[str, Any], timeout_s: int, fallback_skip: bool) -> Tuple[bool, str, Dict[str, Any]]:
    """Run C++ decoder; optionally retry with --skip-syncword; return (ok, hex, diag)."""
    # Defines the function _run.
    def _run(cmd: List[str]) -> Tuple[bool, str, Dict[str, Any]]:
        # Begins a block that monitors for exceptions.
        try:
            # Executes the statement `proc = run(cmd, capture_output=True, text=True, timeout=timeout_s)`.
            proc = run(cmd, capture_output=True, text=True, timeout=timeout_s)
        # Handles a specific exception from the try block.
        except TimeoutExpired:
            # Returns the computed value to the caller.
            return False, '', {'status': 'timeout', 'timeout_sec': timeout_s}
        # Begins a conditional branch to check a condition.
        if proc.returncode != 0:
            # Returns the computed value to the caller.
            return False, '', {
                # Executes the statement `'status': 'error', 'ret': proc.returncode,`.
                'status': 'error', 'ret': proc.returncode,
                # Executes the statement `'stderr': proc.stderr[-400:], 'stdout': proc.stdout[-400:],`.
                'stderr': proc.stderr[-400:], 'stdout': proc.stdout[-400:],
            # Closes the previously opened dictionary or set literal.
            }
        # Executes the statement `hexval = ''`.
        hexval = ''
        # Starts a loop iterating over a sequence.
        for line in proc.stdout.splitlines():
            # Begins a conditional branch to check a condition.
            if line.startswith('payload_hex='):
                # Executes the statement `hexval = line.split('=', 1)[1].strip().lower()`.
                hexval = line.split('=', 1)[1].strip().lower()
                # Exits the nearest enclosing loop early.
                break
        # Returns the computed value to the caller.
        return True, hexval, {'status': 'ok'}

    # Executes the statement `ok, hexval, diag = _run(make_cpp_cmd(cf32, meta, skip_syncword=False))`.
    ok, hexval, diag = _run(make_cpp_cmd(cf32, meta, skip_syncword=False))
    # Begins a conditional branch to check a condition.
    if ok:
        # Returns the computed value to the caller.
        return ok, hexval, diag
    # Begins a conditional branch to check a condition.
    if fallback_skip:
        # Executes the statement `ok2, hex2, diag2 = _run(make_cpp_cmd(cf32, meta, skip_syncword=True))`.
        ok2, hex2, diag2 = _run(make_cpp_cmd(cf32, meta, skip_syncword=True))
        # Begins a conditional branch to check a condition.
        if ok2:
            # Returns the computed value to the caller.
            return ok2, hex2, {'status': 'ok', 'used_skip_syncword': True}
        # Returns the computed value to the caller.
        return ok2, hex2, {'first': diag, 'second': diag2}
    # Returns the computed value to the caller.
    return ok, hexval, diag


# Defines the function collect_vectors.
def collect_vectors(roots: List[Path]) -> List[Path]:
    # Executes the statement `vecs: List[Path] = []`.
    vecs: List[Path] = []
    # Starts a loop iterating over a sequence.
    for r in roots:
        # Executes the statement `base = (ROOT / r).resolve()`.
        base = (ROOT / r).resolve()
        # Begins a conditional branch to check a condition.
        if not base.exists():
            # Skips to the next iteration of the loop.
            continue
        # Executes the statement `vecs.extend(sorted(base.glob('*.cf32')))`.
        vecs.extend(sorted(base.glob('*.cf32')))
    # Returns the computed value to the caller.
    return vecs


# Defines the function categorize.
def categorize(py_info: Dict[str, Any] | None, gnur_info: Dict[str, Any] | None, cpp_ok: bool, cpp_hex: str) -> str:
    # Executes the statement `py_hex = ((py_info or {}).get('payload_hex') or '').lower()`.
    py_hex = ((py_info or {}).get('payload_hex') or '').lower()
    # Executes the statement `gnur_payloads = [p.lower() for p in ((gnur_info or {}).get('payloads') or [])] if (gnur_info or {}).get('status') == 'ok' else []`.
    gnur_payloads = [p.lower() for p in ((gnur_info or {}).get('payloads') or [])] if (gnur_info or {}).get('status') == 'ok' else []

    # Executes the statement `py_ok = bool(py_hex)`.
    py_ok = bool(py_hex)
    # Executes the statement `gnur_ok = bool(gnur_payloads)`.
    gnur_ok = bool(gnur_payloads)
    # Executes the statement `cpp_match_py = (cpp_hex == py_hex) if (py_ok and cpp_ok) else False`.
    cpp_match_py = (cpp_hex == py_hex) if (py_ok and cpp_ok) else False
    # Executes the statement `cpp_match_gnur = (cpp_hex in gnur_payloads) if (gnur_ok and cpp_ok) else False`.
    cpp_match_gnur = (cpp_hex in gnur_payloads) if (gnur_ok and cpp_ok) else False

    # Begins a conditional branch to check a condition.
    if py_ok and cpp_ok and gnur_ok and cpp_match_py and cpp_match_gnur:
        # Returns the computed value to the caller.
        return 'all_match'
    # Begins a conditional branch to check a condition.
    if py_ok and cpp_ok and cpp_match_py and not gnur_ok:
        # Returns the computed value to the caller.
        return 'cpp_python_only'
    # Begins a conditional branch to check a condition.
    if cpp_ok and gnur_ok and cpp_match_gnur and not py_ok:
        # Returns the computed value to the caller.
        return 'cpp_gnur_only'
    # Begins a conditional branch to check a condition.
    if py_ok and gnur_ok and (py_hex in gnur_payloads) and not cpp_ok:
        # Returns the computed value to the caller.
        return 'python_gnur_only'
    # Begins a conditional branch to check a condition.
    if cpp_ok and py_ok and not cpp_match_py:
        # Returns the computed value to the caller.
        return 'cpp_vs_python_mismatch'
    # Begins a conditional branch to check a condition.
    if cpp_ok and gnur_ok and not cpp_match_gnur:
        # Returns the computed value to the caller.
        return 'cpp_vs_gnur_mismatch'
    # Begins a conditional branch to check a condition.
    if py_ok and gnur_ok and (py_hex not in gnur_payloads):
        # Returns the computed value to the caller.
        return 'python_vs_gnur_mismatch'
    # Begins a conditional branch to check a condition.
    if cpp_ok and not (py_ok or gnur_ok):
        # Returns the computed value to the caller.
        return 'cpp_only'
    # Begins a conditional branch to check a condition.
    if py_ok and not (cpp_ok or gnur_ok):
        # Returns the computed value to the caller.
        return 'python_only'
    # Begins a conditional branch to check a condition.
    if gnur_ok and not (cpp_ok or py_ok):
        # Returns the computed value to the caller.
        return 'gnur_only'
    # Returns the computed value to the caller.
    return 'all_fail'


# Defines the function main.
def main() -> None:
    # Executes the statement `ap = argparse.ArgumentParser(description='Robust cross-receiver comparison (v2)')`.
    ap = argparse.ArgumentParser(description='Robust cross-receiver comparison (v2)')
    # Executes the statement `ap.add_argument('--roots', nargs='*', default=None, help='Vector roots (relative to repo root)')`.
    ap.add_argument('--roots', nargs='*', default=None, help='Vector roots (relative to repo root)')
    # Executes the statement `ap.add_argument('--limit', type=int, default=None, help='Limit number of vectors')`.
    ap.add_argument('--limit', type=int, default=None, help='Limit number of vectors')
    # Executes the statement `ap.add_argument('--timeout', type=int, default=45, help='Per-vector timeout (seconds) for C++')`.
    ap.add_argument('--timeout', type=int, default=45, help='Per-vector timeout (seconds) for C++')
    # Executes the statement `ap.add_argument('--fallback-skip', action='store_true', help='Fallback to --skip-syncword on C++ failure')`.
    ap.add_argument('--fallback-skip', action='store_true', help='Fallback to --skip-syncword on C++ failure')
    # Executes the statement `ap.add_argument('--output', default='results/receiver_comparison_v2.json', help='Output JSON path')`.
    ap.add_argument('--output', default='results/receiver_comparison_v2.json', help='Output JSON path')
    # Executes the statement `ap.add_argument('--progress-every', type=int, default=1, help='Print progress every N vectors (default 1)')`.
    ap.add_argument('--progress-every', type=int, default=1, help='Print progress every N vectors (default 1)')
    # Executes the statement `args = ap.parse_args()`.
    args = ap.parse_args()

    # Executes the statement `py_truth = load_truth(PY_TRUTH)`.
    py_truth = load_truth(PY_TRUTH)
    # Executes the statement `gnur_truth = load_truth(GNUR_TRUTH)`.
    gnur_truth = load_truth(GNUR_TRUTH)

    # Executes the statement `roots = [Path(r) for r in args.roots] if args.roots else DEFAULT_ROOTS`.
    roots = [Path(r) for r in args.roots] if args.roots else DEFAULT_ROOTS
    # Executes the statement `vectors = collect_vectors(roots)`.
    vectors = collect_vectors(roots)
    # Begins a conditional branch to check a condition.
    if args.limit:
        # Executes the statement `vectors = vectors[: args.limit]`.
        vectors = vectors[: args.limit]

    # Executes the statement `counts: Dict[str, int] = {}`.
    counts: Dict[str, int] = {}
    # Executes the statement `entries: Dict[str, Any] = {}`.
    entries: Dict[str, Any] = {}

    # Starts a loop iterating over a sequence.
    for idx, cf32 in enumerate(vectors, start=1):
        # Executes the statement `rel = str(cf32.relative_to(ROOT))`.
        rel = str(cf32.relative_to(ROOT))
        # Begins a block that monitors for exceptions.
        try:
            # Executes the statement `meta = load_meta(cf32)`.
            meta = load_meta(cf32)
        # Handles a specific exception from the try block.
        except Exception as e:
            # Executes the statement `entries[rel] = {'status': 'meta_error', 'error': repr(e)}`.
            entries[rel] = {'status': 'meta_error', 'error': repr(e)}
            # Executes the statement `counts['meta_error'] = counts.get('meta_error', 0) + 1`.
            counts['meta_error'] = counts.get('meta_error', 0) + 1
            # Skips to the next iteration of the loop.
            continue

        # Executes the statement `cpp_ok, cpp_hex, diag = run_cpp_with_timeout(cf32, meta, timeout_s=args.timeout, fallback_skip=args.fallback_skip)`.
        cpp_ok, cpp_hex, diag = run_cpp_with_timeout(cf32, meta, timeout_s=args.timeout, fallback_skip=args.fallback_skip)
        # Executes the statement `py_info = py_truth.get(rel)`.
        py_info = py_truth.get(rel)
        # Executes the statement `gnur_info = gnur_truth.get(rel)`.
        gnur_info = gnur_truth.get(rel)
        # Executes the statement `cat = categorize(py_info, gnur_info, cpp_ok, cpp_hex)`.
        cat = categorize(py_info, gnur_info, cpp_ok, cpp_hex)
        # Executes the statement `counts[cat] = counts.get(cat, 0) + 1`.
        counts[cat] = counts.get(cat, 0) + 1

        # Executes the statement `entries[rel] = {`.
        entries[rel] = {
            # Executes the statement `'sf': meta.get('sf'),`.
            'sf': meta.get('sf'),
            # Executes the statement `'cr': meta.get('cr'),`.
            'cr': meta.get('cr'),
            # Executes the statement `'ldro_mode': meta.get('ldro_mode'),`.
            'ldro_mode': meta.get('ldro_mode'),
            # Executes the statement `'impl_header': bool(meta.get('impl_header', False)),`.
            'impl_header': bool(meta.get('impl_header', False)),
            # Executes the statement `'python': py_info or {'status': 'missing'},`.
            'python': py_info or {'status': 'missing'},
            # Executes the statement `'gnuradio': gnur_info or {'status': 'missing'},`.
            'gnuradio': gnur_info or {'status': 'missing'},
            # Executes the statement `'cpp_ok': cpp_ok,`.
            'cpp_ok': cpp_ok,
            # Executes the statement `'cpp_payload_hex': cpp_hex,`.
            'cpp_payload_hex': cpp_hex,
            # Executes the statement `'cpp_diag': diag,`.
            'cpp_diag': diag,
            # Executes the statement `'category': cat,`.
            'category': cat,
        # Closes the previously opened dictionary or set literal.
        }

        # Begins a conditional branch to check a condition.
        if (args.progress_every and (idx % args.progress_every == 0)) or idx == len(vectors):
            # Outputs diagnostic or user-facing text.
            print(f"[{idx}/{len(vectors)}] {cf32.name} -> {cat}")
            # Executes the statement `sys.stdout.flush()`.
            sys.stdout.flush()

    # Executes the statement `out_path = (ROOT / args.output).resolve()`.
    out_path = (ROOT / args.output).resolve()
    # Executes the statement `out_path.parent.mkdir(parents=True, exist_ok=True)`.
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Executes the statement `out_path.write_text(json.dumps({'counts': counts, 'entries': entries}, indent=2))`.
    out_path.write_text(json.dumps({'counts': counts, 'entries': entries}, indent=2))
    # Outputs diagnostic or user-facing text.
    print(json.dumps(counts, indent=2))


# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()


