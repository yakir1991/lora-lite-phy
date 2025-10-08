#!/usr/bin/env python3
# This file provides the 'compare all receivers' functionality for the LoRa Lite PHY toolkit.
"""Compare payload outputs of C++, Python, and GNU Radio receivers."""

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from subprocess import run, TimeoutExpired'.
from subprocess import run, TimeoutExpired
# Imports specific objects with 'from typing import Dict, Any, Tuple'.
from typing import Dict, Any, Tuple

# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `PY_TRUTH = ROOT / 'results/python_truth.json'`.
PY_TRUTH = ROOT / 'results/python_truth.json'
# Executes the statement `GNUR_TRUTH = ROOT / 'results/gnur_truth.json'`.
GNUR_TRUTH = ROOT / 'results/gnur_truth.json'


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


# Defines the function run_cpp.
def run_cpp(cf32: Path, meta: Dict[str, Any], timeout_s: int | None) -> Tuple[bool, str, str]:
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
    # Executes the statement `cmd.append(str(cf32))`.
    cmd.append(str(cf32))
    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `proc = run(cmd, capture_output=True, text=True, timeout=timeout_s)`.
        proc = run(cmd, capture_output=True, text=True, timeout=timeout_s)
    # Handles a specific exception from the try block.
    except TimeoutExpired:
        # Returns the computed value to the caller.
        return False, '', f'timeout after {timeout_s}s'
    # Begins a conditional branch to check a condition.
    if proc.returncode != 0:
        # Returns the computed value to the caller.
        return False, '', proc.stderr
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
    return True, hexval, ''


# Defines the function load_meta.
def load_meta(cf32: Path) -> Dict[str, Any]:
    # Executes the statement `meta_path = cf32.with_suffix('.json')`.
    meta_path = cf32.with_suffix('.json')
    # Returns the computed value to the caller.
    return json.loads(meta_path.read_text())


# Defines the function categorize.
def categorize(py_info: Dict[str, Any], gnur_info: Dict[str, Any], cpp_ok: bool, cpp_hex: str) -> str:
    # Executes the statement `py_hex = (py_info.get('payload_hex') or '').lower()`.
    py_hex = (py_info.get('payload_hex') or '').lower()
    # Executes the statement `gnur_payloads = [p.lower() for p in gnur_info.get('payloads', [])] if gnur_info.get('status') == 'ok' else []`.
    gnur_payloads = [p.lower() for p in gnur_info.get('payloads', [])] if gnur_info.get('status') == 'ok' else []

    # Executes the statement `py_ok = bool(py_hex)`.
    py_ok = bool(py_hex)
    # Executes the statement `gnur_ok = bool(gnur_payloads)`.
    gnur_ok = bool(gnur_payloads)
    # Executes the statement `cpp_match_py = cpp_hex == py_hex if (py_ok and cpp_ok) else False`.
    cpp_match_py = cpp_hex == py_hex if (py_ok and cpp_ok) else False
    # Executes the statement `cpp_match_gnur = cpp_hex in gnur_payloads if (gnur_ok and cpp_ok) else False`.
    cpp_match_gnur = cpp_hex in gnur_payloads if (gnur_ok and cpp_ok) else False

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
    if py_ok and gnur_ok and py_hex in gnur_payloads and not cpp_ok:
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
    if py_ok and gnur_ok and py_hex not in gnur_payloads:
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
def main():
    # Executes the statement `parser = argparse.ArgumentParser(description='Compare outputs from all receivers')`.
    parser = argparse.ArgumentParser(description='Compare outputs from all receivers')
    # Configures the argument parser for the CLI.
    parser.add_argument('--output', default='results/receiver_comparison.json', help='Output summary JSON path')
    # Configures the argument parser for the CLI.
    parser.add_argument('--limit', type=int, help='Limit number of vectors for quick tests')
    # Configures the argument parser for the CLI.
    parser.add_argument('--timeout', type=int, default=60, help='Per-vector timeout for C++ run (seconds)')
    # Executes the statement `args = parser.parse_args()`.
    args = parser.parse_args()

    # Executes the statement `python_truth = json.loads(PY_TRUTH.read_text()) if PY_TRUTH.exists() else {}`.
    python_truth = json.loads(PY_TRUTH.read_text()) if PY_TRUTH.exists() else {}
    # Executes the statement `gnur_truth = json.loads(GNUR_TRUTH.read_text()) if GNUR_TRUTH.exists() else {}`.
    gnur_truth = json.loads(GNUR_TRUTH.read_text()) if GNUR_TRUTH.exists() else {}

    # Executes the statement `vectors = sorted(python_truth.keys() | gnur_truth.keys())`.
    vectors = sorted(python_truth.keys() | gnur_truth.keys())
    # Begins a conditional branch to check a condition.
    if args.limit:
        # Executes the statement `vectors = vectors[:args.limit]`.
        vectors = vectors[:args.limit]

    # Executes the statement `summary: Dict[str, Any] = {}`.
    summary: Dict[str, Any] = {}
    # Executes the statement `counts: Dict[str, int] = {}`.
    counts: Dict[str, int] = {}

    # Starts a loop iterating over a sequence.
    for rel in vectors:
        # Executes the statement `cf32 = (ROOT / rel).resolve()`.
        cf32 = (ROOT / rel).resolve()
        # Executes the statement `meta = load_meta(cf32)`.
        meta = load_meta(cf32)
        # Executes the statement `py_info = python_truth.get(rel, {'status': 'missing'})`.
        py_info = python_truth.get(rel, {'status': 'missing'})
        # Executes the statement `gnur_info = gnur_truth.get(rel, {'status': 'missing'})`.
        gnur_info = gnur_truth.get(rel, {'status': 'missing'})

        # Executes the statement `cpp_ok, cpp_hex, cpp_err = run_cpp(cf32, meta, timeout_s=args.timeout)`.
        cpp_ok, cpp_hex, cpp_err = run_cpp(cf32, meta, timeout_s=args.timeout)
        # Executes the statement `category = categorize(py_info, gnur_info, cpp_ok, cpp_hex)`.
        category = categorize(py_info, gnur_info, cpp_ok, cpp_hex)
        # Executes the statement `counts[category] = counts.get(category, 0) + 1`.
        counts[category] = counts.get(category, 0) + 1

        # Executes the statement `entry = {`.
        entry = {
            # Executes the statement `'sf': meta.get('sf'),`.
            'sf': meta.get('sf'),
            # Executes the statement `'cr': meta.get('cr'),`.
            'cr': meta.get('cr'),
            # Executes the statement `'ldro_mode': meta.get('ldro_mode'),`.
            'ldro_mode': meta.get('ldro_mode'),
            # Executes the statement `'impl_header': bool(meta.get('impl_header', False)),`.
            'impl_header': bool(meta.get('impl_header', False)),
            # Executes the statement `'python': py_info,`.
            'python': py_info,
            # Executes the statement `'gnuradio': gnur_info,`.
            'gnuradio': gnur_info,
            # Executes the statement `'cpp_ok': cpp_ok,`.
            'cpp_ok': cpp_ok,
            # Executes the statement `'cpp_payload_hex': cpp_hex,`.
            'cpp_payload_hex': cpp_hex,
            # Executes the statement `'cpp_error': cpp_err if not cpp_ok else None,`.
            'cpp_error': cpp_err if not cpp_ok else None,
            # Executes the statement `'category': category,`.
            'category': category,
        # Closes the previously opened dictionary or set literal.
        }
        # Executes the statement `summary[rel] = entry`.
        summary[rel] = entry

    # Executes the statement `out_path = (ROOT / args.output).resolve()`.
    out_path = (ROOT / args.output).resolve()
    # Executes the statement `out_path.parent.mkdir(parents=True, exist_ok=True)`.
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Executes the statement `out_path.write_text(json.dumps({'counts': counts, 'entries': summary}, indent=2))`.
    out_path.write_text(json.dumps({'counts': counts, 'entries': summary}, indent=2))
    # Outputs diagnostic or user-facing text.
    print(f'Wrote summary to {out_path}')
    # Outputs diagnostic or user-facing text.
    print(json.dumps(counts, indent=2))


# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
