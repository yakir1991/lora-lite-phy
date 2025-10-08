#!/usr/bin/env python3
# This file provides the 'generate gnur truth' functionality for the LoRa Lite PHY toolkit.
"""Run GNU Radio offline decoder on vectors to collect payload truth."""

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
# Imports specific objects with 'from typing import Dict, Any, List, Tuple'.
from typing import Dict, Any, List, Tuple

# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `SCRIPT = ROOT / 'external' / 'gr_lora_sdr' / 'scripts' / 'decode_offline_recording.py'`.
SCRIPT = ROOT / 'external' / 'gr_lora_sdr' / 'scripts' / 'decode_offline_recording.py'
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


# Defines the function load_meta.
def load_meta(cf32: Path) -> Dict[str, Any]:
    # Executes the statement `meta_path = cf32.with_suffix('.json')`.
    meta_path = cf32.with_suffix('.json')
    # Begins a conditional branch to check a condition.
    if not meta_path.exists():
        # Raises an exception to signal an error.
        raise FileNotFoundError(f'Metadata JSON missing for {cf32}')
    # Returns the computed value to the caller.
    return json.loads(meta_path.read_text())


# Defines the function gnur_cmd.
def gnur_cmd(cf32: Path, meta: Dict[str, Any]) -> List[str]:
    # Executes the statement `samp_rate = int(meta.get('samp_rate') or meta.get('sample_rate') or meta.get('fs') or 0)`.
    samp_rate = int(meta.get('samp_rate') or meta.get('sample_rate') or meta.get('fs') or 0)
    # Begins a conditional branch to check a condition.
    if samp_rate <= 0:
        # Executes the statement `samp_rate = int(meta.get('bw', 0))`.
        samp_rate = int(meta.get('bw', 0))
    # Executes the statement `cmd = [`.
    cmd = [
        # Executes the statement `'conda', 'run', '-n', 'gr310', 'python', str(SCRIPT), str(cf32),`.
        'conda', 'run', '-n', 'gr310', 'python', str(SCRIPT), str(cf32),
        # Executes the statement `'--sf', str(meta.get('sf')),`.
        '--sf', str(meta.get('sf')),
        # Executes the statement `'--bw', str(meta.get('bw')),`.
        '--bw', str(meta.get('bw')),
        # Executes the statement `'--samp-rate', str(samp_rate),`.
        '--samp-rate', str(samp_rate),
        # Executes the statement `'--cr', str(meta.get('cr', 1)),`.
        '--cr', str(meta.get('cr', 1)),
        # Executes the statement `'--pay-len', str(meta.get('payload_len', 255)),`.
        '--pay-len', str(meta.get('payload_len', 255)),
        # Executes the statement `'--ldro-mode', str(meta.get('ldro_mode', 2)),`.
        '--ldro-mode', str(meta.get('ldro_mode', 2)),
        # Executes the statement `'--sync-word', hex(int(meta.get('sync_word', 0x12))),`.
        '--sync-word', hex(int(meta.get('sync_word', 0x12))),
    # Closes the previously opened list indexing or literal.
    ]
    # Begins a conditional branch to check a condition.
    if meta.get('crc', True):
        # Executes the statement `cmd.append('--has-crc')`.
        cmd.append('--has-crc')
    # Provides the fallback branch when previous conditions fail.
    else:
        # Executes the statement `cmd.append('--no-crc')`.
        cmd.append('--no-crc')
    # Begins a conditional branch to check a condition.
    if meta.get('impl_header', False):
        # Executes the statement `cmd.append('--impl-header')`.
        cmd.append('--impl-header')
    # Provides the fallback branch when previous conditions fail.
    else:
        # Executes the statement `cmd.append('--explicit-header')`.
        cmd.append('--explicit-header')
    # Returns the computed value to the caller.
    return cmd


# Defines the function run_gnur.
def run_gnur(cf32: Path, meta: Dict[str, Any], timeout: int) -> Dict[str, Any]:
    # Begins a conditional branch to check a condition.
    if not SCRIPT.exists():
        # Returns the computed value to the caller.
        return {'status': 'error', 'stderr': f'script {SCRIPT} missing'}
    # Executes the statement `cmd = gnur_cmd(cf32, meta)`.
    cmd = gnur_cmd(cf32, meta)
    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)`.
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    # Handles a specific exception from the try block.
    except subprocess.TimeoutExpired as exc:
        # Executes the statement `stderr = exc.stderr.decode() if isinstance(exc.stderr, bytes) else (exc.stderr or '')`.
        stderr = exc.stderr.decode() if isinstance(exc.stderr, bytes) else (exc.stderr or '')
        # Returns the computed value to the caller.
        return {'status': 'timeout', 'stderr': stderr}
    # Begins a conditional branch to check a condition.
    if proc.returncode != 0:
        # Returns the computed value to the caller.
        return {
            # Executes the statement `'status': 'error',`.
            'status': 'error',
            # Executes the statement `'returncode': proc.returncode,`.
            'returncode': proc.returncode,
            # Executes the statement `'stdout': proc.stdout,`.
            'stdout': proc.stdout,
            # Executes the statement `'stderr': proc.stderr,`.
            'stderr': proc.stderr,
        # Closes the previously opened dictionary or set literal.
        }
    # Executes the statement `payloads: List[str] = []`.
    payloads: List[str] = []
    # Executes the statement `crc: List[str] = []`.
    crc: List[str] = []
    # Starts a loop iterating over a sequence.
    for line in proc.stdout.splitlines():
        # Executes the statement `line = line.strip()`.
        line = line.strip()
        # Begins a conditional branch to check a condition.
        if line.startswith('Frame '):
            # Begins a conditional branch to check a condition.
            if 'CRC' in line:
                # Executes the statement `crc_state = line.split('CRC', 1)[1].strip()`.
                crc_state = line.split('CRC', 1)[1].strip()
                # Executes the statement `crc.append(crc_state)`.
                crc.append(crc_state)
        # Begins a conditional branch to check a condition.
        if line.startswith('Hex:'):
            # Executes the statement `hex_tokens = line.split(':', 1)[1].strip().split()`.
            hex_tokens = line.split(':', 1)[1].strip().split()
            # Executes the statement `payloads.append(''.join(hex_tokens).lower())`.
            payloads.append(''.join(hex_tokens).lower())
    # Begins a conditional branch to check a condition.
    if not payloads:
        # Returns the computed value to the caller.
        return {
            # Executes the statement `'status': 'ok',`.
            'status': 'ok',
            # Executes the statement `'payloads': [],`.
            'payloads': [],
            # Executes the statement `'stdout': proc.stdout,`.
            'stdout': proc.stdout,
        # Closes the previously opened dictionary or set literal.
        }
    # Returns the computed value to the caller.
    return {
        # Executes the statement `'status': 'ok',`.
        'status': 'ok',
        # Executes the statement `'payloads': payloads,`.
        'payloads': payloads,
        # Executes the statement `'crc': crc,`.
        'crc': crc,
        # Executes the statement `'stdout': proc.stdout,`.
        'stdout': proc.stdout,
    # Closes the previously opened dictionary or set literal.
    }


# Defines the function collect_vectors.
def collect_vectors(roots: List[Path]) -> List[Path]:
    # Executes the statement `vectors: List[Path] = []`.
    vectors: List[Path] = []
    # Starts a loop iterating over a sequence.
    for root in roots:
        # Executes the statement `base = (ROOT / root).resolve()`.
        base = (ROOT / root).resolve()
        # Begins a conditional branch to check a condition.
        if not base.exists():
            # Skips to the next iteration of the loop.
            continue
        # Executes the statement `vectors.extend(sorted(base.glob('*.cf32')))`.
        vectors.extend(sorted(base.glob('*.cf32')))
    # Returns the computed value to the caller.
    return vectors


# Defines the function main.
def main():
    # Executes the statement `parser = argparse.ArgumentParser(description='Generate GNU Radio payload truth for vectors')`.
    parser = argparse.ArgumentParser(description='Generate GNU Radio payload truth for vectors')
    # Configures the argument parser for the CLI.
    parser.add_argument('--roots', nargs='*', default=None,
                        # Executes the statement `help='Vector directories (relative to repo root); default covers golden sets')`.
                        help='Vector directories (relative to repo root); default covers golden sets')
    # Configures the argument parser for the CLI.
    parser.add_argument('--output', default='results/gnur_truth.json', help='Output JSON file')
    # Configures the argument parser for the CLI.
    parser.add_argument('--timeout', type=int, default=180, help='Timeout per vector (seconds)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--limit', type=int, help='Limit number of vectors for quick tests')
    # Executes the statement `args = parser.parse_args()`.
    args = parser.parse_args()

    # Executes the statement `roots = [Path(r) for r in args.roots] if args.roots else DEFAULT_ROOTS`.
    roots = [Path(r) for r in args.roots] if args.roots else DEFAULT_ROOTS
    # Executes the statement `vectors = collect_vectors(roots)`.
    vectors = collect_vectors(roots)
    # Begins a conditional branch to check a condition.
    if args.limit:
        # Executes the statement `vectors = vectors[:args.limit]`.
        vectors = vectors[:args.limit]

    # Executes the statement `results: Dict[str, Any] = {}`.
    results: Dict[str, Any] = {}
    # Executes the statement `out_path = (ROOT / args.output).resolve()`.
    out_path = (ROOT / args.output).resolve()
    # Begins a conditional branch to check a condition.
    if out_path.exists():
        # Executes the statement `results = json.loads(out_path.read_text())`.
        results = json.loads(out_path.read_text())

    # Starts a loop iterating over a sequence.
    for cf32 in vectors:
        # Executes the statement `rel = str(cf32.relative_to(ROOT))`.
        rel = str(cf32.relative_to(ROOT))
        # Executes the statement `meta = load_meta(cf32)`.
        meta = load_meta(cf32)
        # Executes the statement `res = run_gnur(cf32, meta, timeout=args.timeout)`.
        res = run_gnur(cf32, meta, timeout=args.timeout)
        # Executes the statement `results[rel] = {`.
        results[rel] = {
            # Executes the statement `'sf': meta.get('sf'),`.
            'sf': meta.get('sf'),
            # Executes the statement `'bw': meta.get('bw'),`.
            'bw': meta.get('bw'),
            # Executes the statement `'cr': meta.get('cr'),`.
            'cr': meta.get('cr'),
            # Executes the statement `'ldro_mode': meta.get('ldro_mode'),`.
            'ldro_mode': meta.get('ldro_mode'),
            # Executes the statement `'impl_header': bool(meta.get('impl_header', False)),`.
            'impl_header': bool(meta.get('impl_header', False)),
            # Executes the statement `**res,`.
            **res,
        # Closes the previously opened dictionary or set literal.
        }
        # Outputs diagnostic or user-facing text.
        print(f"[gnur] {rel}: {res.get('status')}")

    # Executes the statement `out_path.parent.mkdir(parents=True, exist_ok=True)`.
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Executes the statement `out_path.write_text(json.dumps(results, indent=2))`.
    out_path.write_text(json.dumps(results, indent=2))
    # Outputs diagnostic or user-facing text.
    print(f"Wrote {len(results)} entries -> {out_path}")


# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
