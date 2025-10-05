#!/usr/bin/env python3
"""Benchmark C++ vs Python (and optional GNU Radio) receivers.

Measures wall time, CPU time, and (when available) max RSS per run.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Dict, Any, List

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TRUTH = ROOT / 'results/python_truth.json'
GR_SCRIPT = ROOT / 'external' / 'gr_lora_sdr' / 'scripts' / 'decode_offline_recording.py'


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


def make_cpp_cmd(cf32: Path, meta: Dict[str, Any], skip_syncword: bool = False) -> List[str]:
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
            '--payload-len', str(meta.get('payload_len', 0)),
            '--cr', str(meta.get('cr', 1)),
            '--has-crc' if meta.get('crc', True) else '--no-crc',
        ]
    # Respect metadata sync_word if present
    sw = meta.get('sync_word')
    if sw is not None:
        cmd += ['--sync-word', hex(int(sw))]
    if skip_syncword:
        cmd.append('--skip-syncword')
    cmd.append(str(cf32))
    return cmd


def make_py_cmd(cf32: Path, fast: bool) -> List[str]:
    cmd = ['python', '-m', 'scripts.sdr_lora_cli', 'decode', str(cf32)]
    if fast:
        cmd.append('--fast')
    return cmd


def make_gr_cmd(cf32: Path, meta: Dict[str, Any], gr_python: str | None, gr_conda_env: str | None) -> List[str]:
    base = [
        str(GR_SCRIPT),
        str(cf32),
        '--sf', str(meta['sf']),
        '--bw', str(meta['bw']),
        '--samp-rate', str(meta.get('samp_rate') or meta.get('fs') or meta['bw']),
        '--cr', str(meta.get('cr', 1)),
        '--ldro-mode', str(meta.get('ldro_mode', 2)),
        '--format', 'cf32',
    ]
    base.append('--has-crc' if meta.get('crc', True) else '--no-crc')
    base.append('--impl-header' if meta.get('impl_header', False) else '--explicit-header')

    if gr_conda_env:
        # Run under bash login shell to enable conda, then activate env and use python
        activate_cmd = (
            'source "$HOME/miniconda3/etc/profile.d/conda.sh" >/dev/null 2>&1 || '
            'source "$HOME/anaconda3/etc/profile.d/conda.sh" >/dev/null 2>&1 || true; '
            f'conda activate {gr_conda_env}; '
        )
        joined = ' '.join(["python"] + [str(x) for x in base])
        return ['bash', '-lc', activate_cmd + joined]

    py = gr_python or sys.executable
    return [py] + [str(x) for x in base]


def _have_time_cmd() -> str | None:
    for cand in ('/usr/bin/time', shutil.which('time')):
        if cand and os.path.exists(cand):
            return cand
    return None


def _run_once_with_time(cmd: List[str], time_cmd: str | None) -> Dict[str, Any]:
    if time_cmd:
        # Use external time to capture wall, user, sys, and max RSS
        # Emit JSON to a temp file to avoid parsing ambiguities
        fmt = '{"wall":%e,"user":%U,"sys":%S,"max_kb":%M}'
        with tempfile.TemporaryDirectory() as td:
            out_path = Path(td) / 'time.json'
            full = [time_cmd, '-f', fmt, '-o', str(out_path), '--'] + cmd
            proc = subprocess.run(full, capture_output=True, text=True)
            timing = {'wall': None, 'user': None, 'sys': None, 'max_kb': None}
            try:
                data = json.loads(out_path.read_text())
                timing.update(data)
            except Exception:
                pass
            return {
                'returncode': proc.returncode,
                'stdout': proc.stdout,
                'stderr': proc.stderr,
                'timing': timing,
            }
    # Fallback: wall-clock only
    start = time.perf_counter()
    proc = subprocess.run(cmd, capture_output=True, text=True)
    elapsed = time.perf_counter() - start
    return {
        'returncode': proc.returncode,
        'stdout': proc.stdout,
        'stderr': proc.stderr,
        'timing': {'wall': elapsed, 'user': None, 'sys': None, 'max_kb': None},
    }


def time_command(cmd: List[str], runs: int) -> Dict[str, Any]:
    time_cmd = _have_time_cmd()
    walls: List[float] = []
    users: List[float] = []
    syss: List[float] = []
    maxrss: List[int] = []
    last_stdout = None
    last_stderr = None
    for _ in range(runs):
        res = _run_once_with_time(cmd, time_cmd)
        last_stdout = res['stdout']
        last_stderr = res['stderr']
        if res['returncode'] != 0:
            return {
                'status': 'error',
                'returncode': res['returncode'],
                'stderr': last_stderr,
            }
        t = res['timing']
        if t.get('wall') is not None:
            walls.append(float(t['wall']))
        if t.get('user') is not None:
            users.append(float(t['user']))
        if t.get('sys') is not None:
            syss.append(float(t['sys']))
        if t.get('max_kb') is not None:
            try:
                maxrss.append(int(t['max_kb']))
            except Exception:
                pass
    def _avg_std(vals: List[float]) -> tuple[float | None, float | None]:
        if not vals:
            return None, None
        avg = sum(vals) / len(vals)
        std = (sum((v - avg) ** 2 for v in vals) / len(vals)) ** 0.5
        return avg, std
    wall_avg, wall_std = _avg_std(walls)
    user_avg, user_std = _avg_std(users)
    sys_avg, sys_std = _avg_std(syss)
    rss_avg, rss_std = _avg_std([float(x) for x in maxrss])
    return {
        'status': 'ok',
        'avg_sec': wall_avg,
        'std_sec': wall_std,
        'avg_user_sec': user_avg,
        'std_user_sec': user_std,
        'avg_sys_sec': sys_avg,
        'std_sys_sec': sys_std,
        'avg_maxrss_kb': rss_avg,
        'std_maxrss_kb': rss_std,
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
    parser = argparse.ArgumentParser(description='Benchmark C++, Python, and optional GNU Radio receivers')
    parser.add_argument('--truth', default=str(DEFAULT_TRUTH), help='JSON truth file to seed vector list')
    parser.add_argument('--roots', nargs='*', help='Additional vector directories (relative to repo root)')
    parser.add_argument('--runs', type=int, default=3, help='Number of repetitions per receiver (default 3)')
    parser.add_argument('--no-fast', action='store_true', help='Disable --fast flag for Python receiver')
    parser.add_argument('--limit', type=int, help='Limit number of vectors (for quick testing)')
    parser.add_argument('--output', help='Optional JSON output path for benchmarking results')
    parser.add_argument('--with-gr', action='store_true', help='Include GNU Radio offline decoder in benchmark')
    parser.add_argument('--gr-python', type=str, default=None, help='Python interpreter for GNU Radio (optional)')
    parser.add_argument('--gr-conda-env', type=str, default=None, help='Conda env name to activate for GR (alternative to --gr-python)')
    parser.add_argument('--cpp-skip-syncword', action='store_true', help='Always pass --skip-syncword to C++ decoder')
    parser.add_argument('--cpp-fallback-skip-syncword', action='store_true', help='Retry C++ with --skip-syncword if first attempt fails')
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
    total_gr = 0.0
    count_cpp = 0
    count_py = 0
    count_gr = 0

    for cf32 in vectors:
        rel = cf32.relative_to(ROOT)
        meta = load_meta(cf32)

        cpp_res = time_command(make_cpp_cmd(cf32, meta, skip_syncword=args.cpp_skip_syncword), runs)
        py_res = time_command(make_py_cmd(cf32, fast=fast), runs)
        gr_res: Dict[str, Any] | None = None
        if args.with_gr and GR_SCRIPT.exists():
            gr_res = time_command(make_gr_cmd(cf32, meta, args.gr_python, args.gr_conda_env), runs)

        # Optional fallback for C++: retry with --skip-syncword when first attempt fails
        cpp_used_skip = args.cpp_skip_syncword
        if cpp_res['status'] != 'ok' and args.cpp_fallback_skip_syncword and not args.cpp_skip_syncword:
            cpp_res_fb = time_command(make_cpp_cmd(cf32, meta, skip_syncword=True), runs)
            if cpp_res_fb['status'] == 'ok':
                cpp_res = cpp_res_fb
                cpp_used_skip = True

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
            entry['cpp_avg_user_sec'] = cpp_res.get('avg_user_sec')
            entry['cpp_avg_sys_sec'] = cpp_res.get('avg_sys_sec')
            entry['cpp_avg_maxrss_kb'] = cpp_res.get('avg_maxrss_kb')
            entry['cpp_payload_hex'] = parse_payload_hex(cpp_res['stdout'])
            entry['cpp_used_skip_syncword'] = cpp_used_skip
            total_cpp += cpp_res['avg_sec']
            count_cpp += 1
        else:
            entry['cpp_error'] = cpp_res

        if py_res['status'] == 'ok':
            entry['py_avg_sec'] = py_res['avg_sec']
            entry['py_std_sec'] = py_res['std_sec']
            entry['py_avg_user_sec'] = py_res.get('avg_user_sec')
            entry['py_avg_sys_sec'] = py_res.get('avg_sys_sec')
            entry['py_avg_maxrss_kb'] = py_res.get('avg_maxrss_kb')
            entry['python_payload_hex'] = ''
            total_py += py_res['avg_sec']
            count_py += 1
        else:
            entry['py_error'] = py_res

        if gr_res is not None:
            if gr_res['status'] == 'ok':
                entry['gr_avg_sec'] = gr_res['avg_sec']
                entry['gr_std_sec'] = gr_res['std_sec']
                entry['gr_avg_user_sec'] = gr_res.get('avg_user_sec')
                entry['gr_avg_sys_sec'] = gr_res.get('avg_sys_sec')
                entry['gr_avg_maxrss_kb'] = gr_res.get('avg_maxrss_kb')
                # Extract hex from stdout of GR script
                gr_hex = ''
                for line in (gr_res.get('stdout') or '').splitlines():
                    line = line.strip()
                    if line.lower().startswith('hex:'):
                        gr_hex = line.split(':', 1)[1].strip().replace(' ', '').lower()
                if gr_hex:
                    entry['gr_payload_hex'] = gr_hex
                total_gr += gr_res['avg_sec'] or 0.0
                count_gr += 1
            else:
                entry['gr_error'] = gr_res

        results[str(rel)] = entry

    summary = {
        'vector_count': len(results),
        'cpp_avg_total': (total_cpp / count_cpp) if count_cpp else None,
        'py_avg_total': (total_py / count_py) if count_py else None,
        'gr_avg_total': (total_gr / count_gr) if count_gr else None,
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
