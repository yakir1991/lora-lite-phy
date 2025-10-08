#!/usr/bin/env python3
# This file provides the 'generate python truth' functionality for the LoRa Lite PHY toolkit.
"""Generate payload truth table using the python receiver for golden vectors."""

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports the module(s) subprocess.
import subprocess
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from typing import Dict, Any, List'.
from typing import Dict, Any, List

# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
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
    # Executes the statement `Path('golden_vectors/demo'),`.
    Path('golden_vectors/demo'),
    # Executes the statement `Path('golden_vectors/demo_batch'),`.
    Path('golden_vectors/demo_batch'),
    # Executes the statement `Path('golden_vectors/custom'),`.
    Path('golden_vectors/custom'),
# Closes the previously opened list indexing or literal.
]


# Defines the function run_sdr_lora_cli.
def run_sdr_lora_cli(cf32: Path, fast: bool) -> Dict[str, Any]:
    # Executes the statement `cmd = ['python', '-m', 'scripts.sdr_lora_cli', 'decode', str(cf32), '-v']`.
    cmd = ['python', '-m', 'scripts.sdr_lora_cli', 'decode', str(cf32), '-v']
    # Begins a conditional branch to check a condition.
    if fast:
        # Executes the statement `cmd.append('--fast')`.
        cmd.append('--fast')
    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `proc = subprocess.run(cmd, capture_output=True, text=True, timeout=180)`.
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    # Handles a specific exception from the try block.
    except subprocess.TimeoutExpired as exc:
        # Returns the computed value to the caller.
        return {
            # Executes the statement `'status': 'timeout',`.
            'status': 'timeout',
            # Executes the statement `'stderr': exc.stderr.decode() if isinstance(exc.stderr, bytes) else (exc.stderr or ''),`.
            'stderr': exc.stderr.decode() if isinstance(exc.stderr, bytes) else (exc.stderr or ''),
        # Closes the previously opened dictionary or set literal.
        }
    # Begins a conditional branch to check a condition.
    if proc.returncode != 0:
        # Returns the computed value to the caller.
        return {
            # Executes the statement `'status': 'error',`.
            'status': 'error',
            # Executes the statement `'returncode': proc.returncode,`.
            'returncode': proc.returncode,
            # Executes the statement `'stderr': proc.stderr.strip(),`.
            'stderr': proc.stderr.strip(),
        # Closes the previously opened dictionary or set literal.
        }
    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `data = json.loads(proc.stdout)`.
        data = json.loads(proc.stdout)
    # Handles a specific exception from the try block.
    except json.JSONDecodeError as exc:
        # Returns the computed value to the caller.
        return {
            # Executes the statement `'status': 'error',`.
            'status': 'error',
            # Executes the statement `'returncode': proc.returncode,`.
            'returncode': proc.returncode,
            # Executes the statement `'stderr': f'JSON decode failed: {exc}\nRaw: {proc.stdout[:200]}',`.
            'stderr': f'JSON decode failed: {exc}\nRaw: {proc.stdout[:200]}',
        # Closes the previously opened dictionary or set literal.
        }
    # Executes the statement `entry: Dict[str, Any] = {`.
    entry: Dict[str, Any] = {
        # Executes the statement `'status': 'ok',`.
        'status': 'ok',
        # Executes the statement `'sf': data.get('sf'),`.
        'sf': data.get('sf'),
        # Executes the statement `'bw': data.get('bw'),`.
        'bw': data.get('bw'),
        # Executes the statement `'fs': data.get('fs'),`.
        'fs': data.get('fs'),
        # Executes the statement `'expected_hex': (data.get('expected') or '').lower(),`.
        'expected_hex': (data.get('expected') or '').lower(),
        # Executes the statement `'payload_hex': None,`.
        'payload_hex': None,
        # Executes the statement `'count': 0,`.
        'count': 0,
    # Closes the previously opened dictionary or set literal.
    }
    # Executes the statement `found = data.get('found') or []`.
    found = data.get('found') or []
    # Executes the statement `entry['count'] = len(found)`.
    entry['count'] = len(found)
    # Begins a conditional branch to check a condition.
    if found:
        # Executes the statement `entry['payload_hex'] = (found[0].get('hex') or '').lower()`.
        entry['payload_hex'] = (found[0].get('hex') or '').lower()
        # Executes the statement `entry['hdr_ok'] = int(found[0].get('hdr_ok', 0))`.
        entry['hdr_ok'] = int(found[0].get('hdr_ok', 0))
        # Executes the statement `entry['crc_ok'] = int(found[0].get('crc_ok', 0))`.
        entry['crc_ok'] = int(found[0].get('crc_ok', 0))
        # Executes the statement `entry['cr'] = int(found[0].get('cr', 0))`.
        entry['cr'] = int(found[0].get('cr', 0))
        # Executes the statement `entry['ih'] = int(found[0].get('ih', 0))`.
        entry['ih'] = int(found[0].get('ih', 0))
    # Returns the computed value to the caller.
    return entry


# Defines the function collect_truth.
def collect_truth(roots: List[Path], fast: bool) -> Dict[str, Any]:
    # Executes the statement `truth: Dict[str, Any] = {}`.
    truth: Dict[str, Any] = {}
    # Starts a loop iterating over a sequence.
    for root in roots:
        # Executes the statement `base = (ROOT / root).resolve()`.
        base = (ROOT / root).resolve()
        # Begins a conditional branch to check a condition.
        if not base.exists():
            # Skips to the next iteration of the loop.
            continue
        # Starts a loop iterating over a sequence.
        for json_path in sorted(base.glob('*.json')):
            # Executes the statement `cf32_path = json_path.with_suffix('.cf32')`.
            cf32_path = json_path.with_suffix('.cf32')
            # Executes the statement `rel = cf32_path.relative_to(ROOT)`.
            rel = cf32_path.relative_to(ROOT)
            # Begins a conditional branch to check a condition.
            if not cf32_path.exists():
                # Executes the statement `truth[str(rel)] = {'status': 'missing_cf32'}`.
                truth[str(rel)] = {'status': 'missing_cf32'}
                # Skips to the next iteration of the loop.
                continue
            # Executes the statement `truth[str(rel)] = run_sdr_lora_cli(cf32_path, fast=fast)`.
            truth[str(rel)] = run_sdr_lora_cli(cf32_path, fast=fast)
    # Returns the computed value to the caller.
    return truth


# Defines the function main.
def main():
    # Executes the statement `parser = argparse.ArgumentParser(description='Generate python payload truth table')`.
    parser = argparse.ArgumentParser(description='Generate python payload truth table')
    # Configures the argument parser for the CLI.
    parser.add_argument('--roots', nargs='*', default=None,
                        # Executes the statement `help='Root directories (relative to repo) containing vector JSON files')`.
                        help='Root directories (relative to repo) containing vector JSON files')
    # Configures the argument parser for the CLI.
    parser.add_argument('--output', default='results/python_truth.json',
                        # Executes the statement `help='Destination JSON path (relative to repo root)')`.
                        help='Destination JSON path (relative to repo root)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--no-fast', action='store_true',
                        # Executes the statement `help='Disable --fast flag when invoking scripts.sdr_lora_cli (slower, but thorough)')`.
                        help='Disable --fast flag when invoking scripts.sdr_lora_cli (slower, but thorough)')
    # Executes the statement `args = parser.parse_args()`.
    args = parser.parse_args()

    # Begins a conditional branch to check a condition.
    if args.roots:
        # Executes the statement `roots = [Path(r) for r in args.roots]`.
        roots = [Path(r) for r in args.roots]
    # Provides the fallback branch when previous conditions fail.
    else:
        # Executes the statement `roots = DEFAULT_ROOTS`.
        roots = DEFAULT_ROOTS

    # Executes the statement `out_path = (ROOT / args.output).resolve()`.
    out_path = (ROOT / args.output).resolve()
    # Begins a conditional branch to check a condition.
    if out_path.exists():
        # Executes the statement `existing = json.loads(out_path.read_text())`.
        existing = json.loads(out_path.read_text())
    # Provides the fallback branch when previous conditions fail.
    else:
        # Executes the statement `existing = {}`.
        existing = {}

    # Executes the statement `fast = not args.no_fast`.
    fast = not args.no_fast
    # Executes the statement `truth = collect_truth(roots, fast=fast)`.
    truth = collect_truth(roots, fast=fast)
    # Executes the statement `existing.update(truth)`.
    existing.update(truth)

    # Executes the statement `out_path.parent.mkdir(parents=True, exist_ok=True)`.
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Executes the statement `out_path.write_text(json.dumps(existing, indent=2))`.
    out_path.write_text(json.dumps(existing, indent=2))
    # Outputs diagnostic or user-facing text.
    print(f'Updated {out_path} with {len(truth)} entries (total {len(existing)})')


# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
