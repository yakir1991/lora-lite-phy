#!/usr/bin/env python3
"""Benchmark C++ vs Python receivers on selected vectors."""

from __future__ import annotations

import argparse
import json
import subprocess
import time
from pathlib import Path
from typing import Dict, Any, List

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TRUTH = ROOT / 'results/python_truth.json'


def load_meta(cf32: Path) -> Dict[str, Any]:
    meta_path = cf32.with_suffix('.json')
    if not meta_path.exists():
        raise FileNotFoundError(f'Metadata JSON not found for {cf32}')
    return json.loads(meta_path.read_text())


def ldro_flag(meta: Dict[str, Any]) -> str:
    mode = int(meta.get('ldro_mode', 0) or 0)
    sf = int(meta.get('sf', 7))
    if mode == 1:
        return '1'
    if mode == 2 and sf >= 11:
        return '1'
    return '0'


def make_cpp_cmd(cf32: Path, meta: Dict[str, Any]) -> List[str]:
    cmd = [
        str(ROOT / 'cpp_receiver' / 'build' / 'decode_cli'),
        '--sf', str(meta['sf']),
        '--bw', str(meta['bw']),
        '--fs', str(meta.get('samp_rate') or meta.get('fs') or 0),
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
    return cmd


def make_py_cmd(cf32: Path, fast: bool) -> List[str]:
    cmd = ['python', '-m', 'scripts.sdr_lora_cli', 'decode', str(cf32)]
    if fast:
        cmd.append('--fast')
    return cmd


def time_command(cmd: List[str], runs: int) -> Dict[str, Any]:
    timings: List[float] = []
    last_stdout = None
    last_stderr = None
    for _ in range(runs):
        start = time.perf_counter()
        proc = subprocess.run(cmd, capture_output=True, text=True)
        elapsed = time.perf_counter() - start
        timings.append(elapsed)
        last_stdout = proc.stdout
        last_stderr = proc.stderr
        if proc.returncode != 0:
            return {
                'status': 'error',
                'returncode': proc.returncode,
                'stderr': last_stderr,
            }
    avg = sum(timings) / runs
    std = (sum((t - avg) ** 2 for t in timings) / runs) ** 0.5
    return {
        'status': 'ok',
        'avg_sec': avg,
        'std_sec': std,
        'stdout': last_stdout,
    }


def parse_payload_hex(stdout: str) -> str:
    for line in stdout.splitlines():
        if line.startswith('payload_hex='):
            return line.split('=', 1)[1].strip().lower()
    return ''


def collect_vectors(truth_file: Path, roots: List[Path] | None) -> List[Path]:
    vectors: List[Path] = []
    if truth_file.exists():
        data = json.loads(truth_file.read_text())
        for rel in data:
            vectors.append((ROOT / rel).resolve())
    if roots:
        for root in roots:
            base = (ROOT / root).resolve()
            vectors.extend(base.glob('*.cf32'))
    # Remove duplicates while preserving order
    seen = set()
    unique: List[Path] = []
    for cf32 in vectors:
        if cf32 not in seen and cf32.exists():
            unique.append(cf32)
            seen.add(cf32)
    return unique


def main():
    parser = argparse.ArgumentParser(description='Compare runtime of C++ and Python receivers')
    parser.add_argument('--truth', default=str(DEFAULT_TRUTH), help='JSON truth file to seed vector list')
    parser.add_argument('--roots', nargs='*', help='Additional vector directories (relative to repo root)')
    parser.add_argument('--runs', type=int, default=3, help='Number of repetitions per receiver (default 3)')
    parser.add_argument('--no-fast', action='store_true', help='Disable --fast flag for Python receiver')
    parser.add_argument('--limit', type=int, help='Limit number of vectors (for quick testing)')
    parser.add_argument('--output', help='Optional JSON output path for benchmarking results')
    args = parser.parse_args()

    truth_path = Path(args.truth)
    roots = [Path(r) for r in args.roots] if args.roots else None
    vectors = collect_vectors(truth_path, roots)
    if args.limit:
        vectors = vectors[:args.limit]

    runs = max(1, args.runs)
    fast = not args.no_fast

    results: Dict[str, Any] = {}
    total_cpp = 0.0
    total_py = 0.0
    count_cpp = 0
    count_py = 0

    for cf32 in vectors:
        rel = cf32.relative_to(ROOT)
        meta = load_meta(cf32)

        cpp_res = time_command(make_cpp_cmd(cf32, meta), runs)
        py_res = time_command(make_py_cmd(cf32, fast=fast), runs)

        entry: Dict[str, Any] = {
            'sf': meta.get('sf'),
            'bw': meta.get('bw'),
            'cr': meta.get('cr'),
            'impl_header': bool(meta.get('impl_header', False)),
            'ldro_mode': meta.get('ldro_mode', 0),
            'payload_len': meta.get('payload_len'),
        }

        if cpp_res['status'] == 'ok':
            entry['cpp_avg_sec'] = cpp_res['avg_sec']
            entry['cpp_std_sec'] = cpp_res['std_sec']
            entry['cpp_payload_hex'] = parse_payload_hex(cpp_res['stdout'])
            total_cpp += cpp_res['avg_sec']
            count_cpp += 1
        else:
            entry['cpp_error'] = cpp_res

        if py_res['status'] == 'ok':
            entry['py_avg_sec'] = py_res['avg_sec']
            entry['py_std_sec'] = py_res['std_sec']
            entry['python_payload_hex'] = ''
            total_py += py_res['avg_sec']
            count_py += 1
        else:
            entry['py_error'] = py_res

        results[str(rel)] = entry

    summary = {
        'vector_count': len(results),
        'cpp_avg_total': (total_cpp / count_cpp) if count_cpp else None,
        'py_avg_total': (total_py / count_py) if count_py else None,
        'fast_mode': fast,
        'runs_per_vector': runs,
        'entries': results,
    }

    if args.output:
        out_path = (ROOT / args.output).resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(summary, indent=2))
        print(f'Wrote results to {out_path}')
    else:
        print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
