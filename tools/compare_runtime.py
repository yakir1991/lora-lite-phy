#!/usr/bin/env python3
# This file provides the 'compare runtime' functionality for the LoRa Lite PHY toolkit.
"""Benchmark C++ vs Python (and optional GNU Radio) receivers.

Measures wall time, CPU time, and (when available) max RSS per run.
"""

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports the module(s) os.
import os
# Imports the module(s) shutil.
import shutil
# Imports the module(s) subprocess.
import subprocess
# Imports the module(s) sys.
import sys
# Imports the module(s) tempfile.
import tempfile
# Imports the module(s) time.
import time
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from typing import Dict, Any, List'.
from typing import Dict, Any, List

# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `DEFAULT_TRUTH = ROOT / 'results/python_truth.json'`.
DEFAULT_TRUTH = ROOT / 'results/python_truth.json'
# Executes the statement `GR_SCRIPT = ROOT / 'external' / 'gr_lora_sdr' / 'scripts' / 'decode_offline_recording.py'`.
GR_SCRIPT = ROOT / 'external' / 'gr_lora_sdr' / 'scripts' / 'decode_offline_recording.py'


# Defines the function load_meta.
def load_meta(cf32: Path) -> Dict[str, Any]:
    # Executes the statement `meta_path = cf32.with_suffix('.json')`.
    meta_path = cf32.with_suffix('.json')
    # Begins a conditional branch to check a condition.
    if not meta_path.exists():
        # Raises an exception to signal an error.
        raise FileNotFoundError(f'Metadata JSON not found for {cf32}')
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
        # Executes the statement `'--sf', str(meta['sf']),`.
        '--sf', str(meta['sf']),
        # Executes the statement `'--bw', str(meta['bw']),`.
        '--bw', str(meta['bw']),
        # Executes the statement `'--fs', str(meta.get('samp_rate') or meta.get('fs') or 0),`.
        '--fs', str(meta.get('samp_rate') or meta.get('fs') or 0),
        # Executes the statement `'--ldro', ldro_flag(meta),`.
        '--ldro', ldro_flag(meta),
    # Closes the previously opened list indexing or literal.
    ]
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
    # Respect metadata sync_word if present
    # Executes the statement `sw = meta.get('sync_word')`.
    sw = meta.get('sync_word')
    # Begins a conditional branch to check a condition.
    if sw is not None:
        # Executes the statement `cmd += ['--sync-word', hex(int(sw))]`.
        cmd += ['--sync-word', hex(int(sw))]
    # Begins a conditional branch to check a condition.
    if skip_syncword:
        # Executes the statement `cmd.append('--skip-syncword')`.
        cmd.append('--skip-syncword')
    # Executes the statement `cmd.append(str(cf32))`.
    cmd.append(str(cf32))
    # Returns the computed value to the caller.
    return cmd


# Defines the function make_py_cmd.
def make_py_cmd(cf32: Path, fast: bool) -> List[str]:
    # Executes the statement `cmd = ['python', '-m', 'scripts.sdr_lora_cli', 'decode', str(cf32)]`.
    cmd = ['python', '-m', 'scripts.sdr_lora_cli', 'decode', str(cf32)]
    # Begins a conditional branch to check a condition.
    if fast:
        # Executes the statement `cmd.append('--fast')`.
        cmd.append('--fast')
    # Returns the computed value to the caller.
    return cmd


# Defines the function make_gr_cmd.
def make_gr_cmd(cf32: Path, meta: Dict[str, Any], gr_python: str | None, gr_conda_env: str | None) -> List[str]:
    # Executes the statement `base = [`.
    base = [
        # Executes the statement `str(GR_SCRIPT),`.
        str(GR_SCRIPT),
        # Executes the statement `str(cf32),`.
        str(cf32),
        # Executes the statement `'--sf', str(meta['sf']),`.
        '--sf', str(meta['sf']),
        # Executes the statement `'--bw', str(meta['bw']),`.
        '--bw', str(meta['bw']),
        # Executes the statement `'--samp-rate', str(meta.get('samp_rate') or meta.get('fs') or meta['bw']),`.
        '--samp-rate', str(meta.get('samp_rate') or meta.get('fs') or meta['bw']),
        # Executes the statement `'--cr', str(meta.get('cr', 1)),`.
        '--cr', str(meta.get('cr', 1)),
        # Executes the statement `'--ldro-mode', str(meta.get('ldro_mode', 2)),`.
        '--ldro-mode', str(meta.get('ldro_mode', 2)),
        # Executes the statement `'--format', 'cf32',`.
        '--format', 'cf32',
    # Closes the previously opened list indexing or literal.
    ]
    # Executes the statement `base.append('--has-crc' if meta.get('crc', True) else '--no-crc')`.
    base.append('--has-crc' if meta.get('crc', True) else '--no-crc')
    # Executes the statement `base.append('--impl-header' if meta.get('impl_header', False) else '--explicit-header')`.
    base.append('--impl-header' if meta.get('impl_header', False) else '--explicit-header')

    # Begins a conditional branch to check a condition.
    if gr_conda_env:
        # Run under bash login shell to enable conda, then activate env and use python
        # Executes the statement `activate_cmd = (`.
        activate_cmd = (
            # Executes the statement `'source "$HOME/miniconda3/etc/profile.d/conda.sh" >/dev/null 2>&1 || '`.
            'source "$HOME/miniconda3/etc/profile.d/conda.sh" >/dev/null 2>&1 || '
            # Executes the statement `'source "$HOME/anaconda3/etc/profile.d/conda.sh" >/dev/null 2>&1 || true; '`.
            'source "$HOME/anaconda3/etc/profile.d/conda.sh" >/dev/null 2>&1 || true; '
            # Executes the statement `f'conda activate {gr_conda_env}; '`.
            f'conda activate {gr_conda_env}; '
        # Closes the previously opened parenthesis grouping.
        )
        # Executes the statement `joined = ' '.join(["python"] + [str(x) for x in base])`.
        joined = ' '.join(["python"] + [str(x) for x in base])
        # Returns the computed value to the caller.
        return ['bash', '-lc', activate_cmd + joined]

    # Executes the statement `py = gr_python or sys.executable`.
    py = gr_python or sys.executable
    # Returns the computed value to the caller.
    return [py] + [str(x) for x in base]


# Defines the function _have_time_cmd.
def _have_time_cmd() -> str | None:
    # Starts a loop iterating over a sequence.
    for cand in ('/usr/bin/time', shutil.which('time')):
        # Begins a conditional branch to check a condition.
        if cand and os.path.exists(cand):
            # Returns the computed value to the caller.
            return cand
    # Returns the computed value to the caller.
    return None


# Defines the function _run_once_with_time.
def _run_once_with_time(cmd: List[str], time_cmd: str | None) -> Dict[str, Any]:
    # Begins a conditional branch to check a condition.
    if time_cmd:
        # Use external time to capture wall, user, sys, and max RSS
        # Emit JSON to a temp file to avoid parsing ambiguities
        # Executes the statement `fmt = '{"wall":%e,"user":%U,"sys":%S,"max_kb":%M}'`.
        fmt = '{"wall":%e,"user":%U,"sys":%S,"max_kb":%M}'
        # Opens a context manager scope for managed resources.
        with tempfile.TemporaryDirectory() as td:
            # Executes the statement `out_path = Path(td) / 'time.json'`.
            out_path = Path(td) / 'time.json'
            # Executes the statement `full = [time_cmd, '-f', fmt, '-o', str(out_path), '--'] + cmd`.
            full = [time_cmd, '-f', fmt, '-o', str(out_path), '--'] + cmd
            # Executes the statement `proc = subprocess.run(full, capture_output=True, text=True)`.
            proc = subprocess.run(full, capture_output=True, text=True)
            # Executes the statement `timing = {'wall': None, 'user': None, 'sys': None, 'max_kb': None}`.
            timing = {'wall': None, 'user': None, 'sys': None, 'max_kb': None}
            # Begins a block that monitors for exceptions.
            try:
                # Executes the statement `data = json.loads(out_path.read_text())`.
                data = json.loads(out_path.read_text())
                # Executes the statement `timing.update(data)`.
                timing.update(data)
            # Handles a specific exception from the try block.
            except Exception:
                # Acts as a no-operation placeholder statement.
                pass
            # Returns the computed value to the caller.
            return {
                # Executes the statement `'returncode': proc.returncode,`.
                'returncode': proc.returncode,
                # Executes the statement `'stdout': proc.stdout,`.
                'stdout': proc.stdout,
                # Executes the statement `'stderr': proc.stderr,`.
                'stderr': proc.stderr,
                # Executes the statement `'timing': timing,`.
                'timing': timing,
            # Closes the previously opened dictionary or set literal.
            }
    # Fallback: wall-clock only
    # Executes the statement `start = time.perf_counter()`.
    start = time.perf_counter()
    # Executes the statement `proc = subprocess.run(cmd, capture_output=True, text=True)`.
    proc = subprocess.run(cmd, capture_output=True, text=True)
    # Executes the statement `elapsed = time.perf_counter() - start`.
    elapsed = time.perf_counter() - start
    # Returns the computed value to the caller.
    return {
        # Executes the statement `'returncode': proc.returncode,`.
        'returncode': proc.returncode,
        # Executes the statement `'stdout': proc.stdout,`.
        'stdout': proc.stdout,
        # Executes the statement `'stderr': proc.stderr,`.
        'stderr': proc.stderr,
        # Executes the statement `'timing': {'wall': elapsed, 'user': None, 'sys': None, 'max_kb': None},`.
        'timing': {'wall': elapsed, 'user': None, 'sys': None, 'max_kb': None},
    # Closes the previously opened dictionary or set literal.
    }


# Defines the function time_command.
def time_command(cmd: List[str], runs: int) -> Dict[str, Any]:
    # Executes the statement `time_cmd = _have_time_cmd()`.
    time_cmd = _have_time_cmd()
    # Executes the statement `walls: List[float] = []`.
    walls: List[float] = []
    # Executes the statement `users: List[float] = []`.
    users: List[float] = []
    # Executes the statement `syss: List[float] = []`.
    syss: List[float] = []
    # Executes the statement `maxrss: List[int] = []`.
    maxrss: List[int] = []
    # Executes the statement `last_stdout = None`.
    last_stdout = None
    # Executes the statement `last_stderr = None`.
    last_stderr = None
    # Starts a loop iterating over a sequence.
    for _ in range(runs):
        # Executes the statement `res = _run_once_with_time(cmd, time_cmd)`.
        res = _run_once_with_time(cmd, time_cmd)
        # Executes the statement `last_stdout = res['stdout']`.
        last_stdout = res['stdout']
        # Executes the statement `last_stderr = res['stderr']`.
        last_stderr = res['stderr']
        # Begins a conditional branch to check a condition.
        if res['returncode'] != 0:
            # Returns the computed value to the caller.
            return {
                # Executes the statement `'status': 'error',`.
                'status': 'error',
                # Executes the statement `'returncode': res['returncode'],`.
                'returncode': res['returncode'],
                # Executes the statement `'stderr': last_stderr,`.
                'stderr': last_stderr,
            # Closes the previously opened dictionary or set literal.
            }
        # Executes the statement `t = res['timing']`.
        t = res['timing']
        # Begins a conditional branch to check a condition.
        if t.get('wall') is not None:
            # Executes the statement `walls.append(float(t['wall']))`.
            walls.append(float(t['wall']))
        # Begins a conditional branch to check a condition.
        if t.get('user') is not None:
            # Executes the statement `users.append(float(t['user']))`.
            users.append(float(t['user']))
        # Begins a conditional branch to check a condition.
        if t.get('sys') is not None:
            # Executes the statement `syss.append(float(t['sys']))`.
            syss.append(float(t['sys']))
        # Begins a conditional branch to check a condition.
        if t.get('max_kb') is not None:
            # Begins a block that monitors for exceptions.
            try:
                # Executes the statement `maxrss.append(int(t['max_kb']))`.
                maxrss.append(int(t['max_kb']))
            # Handles a specific exception from the try block.
            except Exception:
                # Acts as a no-operation placeholder statement.
                pass
    # Defines the function _avg_std.
    def _avg_std(vals: List[float]) -> tuple[float | None, float | None]:
        # Begins a conditional branch to check a condition.
        if not vals:
            # Returns the computed value to the caller.
            return None, None
        # Executes the statement `avg = sum(vals) / len(vals)`.
        avg = sum(vals) / len(vals)
        # Executes the statement `std = (sum((v - avg) ** 2 for v in vals) / len(vals)) ** 0.5`.
        std = (sum((v - avg) ** 2 for v in vals) / len(vals)) ** 0.5
        # Returns the computed value to the caller.
        return avg, std
    # Executes the statement `wall_avg, wall_std = _avg_std(walls)`.
    wall_avg, wall_std = _avg_std(walls)
    # Executes the statement `user_avg, user_std = _avg_std(users)`.
    user_avg, user_std = _avg_std(users)
    # Executes the statement `sys_avg, sys_std = _avg_std(syss)`.
    sys_avg, sys_std = _avg_std(syss)
    # Executes the statement `rss_avg, rss_std = _avg_std([float(x) for x in maxrss])`.
    rss_avg, rss_std = _avg_std([float(x) for x in maxrss])
    # Returns the computed value to the caller.
    return {
        # Executes the statement `'status': 'ok',`.
        'status': 'ok',
        # Executes the statement `'avg_sec': wall_avg,`.
        'avg_sec': wall_avg,
        # Executes the statement `'std_sec': wall_std,`.
        'std_sec': wall_std,
        # Executes the statement `'avg_user_sec': user_avg,`.
        'avg_user_sec': user_avg,
        # Executes the statement `'std_user_sec': user_std,`.
        'std_user_sec': user_std,
        # Executes the statement `'avg_sys_sec': sys_avg,`.
        'avg_sys_sec': sys_avg,
        # Executes the statement `'std_sys_sec': sys_std,`.
        'std_sys_sec': sys_std,
        # Executes the statement `'avg_maxrss_kb': rss_avg,`.
        'avg_maxrss_kb': rss_avg,
        # Executes the statement `'std_maxrss_kb': rss_std,`.
        'std_maxrss_kb': rss_std,
        # Executes the statement `'stdout': last_stdout,`.
        'stdout': last_stdout,
    # Closes the previously opened dictionary or set literal.
    }


# Defines the function parse_payload_hex.
def parse_payload_hex(stdout: str) -> str:
    # Starts a loop iterating over a sequence.
    for line in stdout.splitlines():
        # Begins a conditional branch to check a condition.
        if line.startswith('payload_hex='):
            # Returns the computed value to the caller.
            return line.split('=', 1)[1].strip().lower()
    # Returns the computed value to the caller.
    return ''


# Defines the function collect_vectors.
def collect_vectors(truth_file: Path, roots: List[Path] | None) -> List[Path]:
    # Executes the statement `vectors: List[Path] = []`.
    vectors: List[Path] = []
    # Begins a conditional branch to check a condition.
    if truth_file.exists():
        # Executes the statement `data = json.loads(truth_file.read_text())`.
        data = json.loads(truth_file.read_text())
        # Starts a loop iterating over a sequence.
        for rel in data:
            # Executes the statement `vectors.append((ROOT / rel).resolve())`.
            vectors.append((ROOT / rel).resolve())
    # Begins a conditional branch to check a condition.
    if roots:
        # Starts a loop iterating over a sequence.
        for root in roots:
            # Executes the statement `base = (ROOT / root).resolve()`.
            base = (ROOT / root).resolve()
            # Executes the statement `vectors.extend(base.glob('*.cf32'))`.
            vectors.extend(base.glob('*.cf32'))
    # Remove duplicates while preserving order
    # Executes the statement `seen = set()`.
    seen = set()
    # Executes the statement `unique: List[Path] = []`.
    unique: List[Path] = []
    # Starts a loop iterating over a sequence.
    for cf32 in vectors:
        # Begins a conditional branch to check a condition.
        if cf32 not in seen and cf32.exists():
            # Executes the statement `unique.append(cf32)`.
            unique.append(cf32)
            # Executes the statement `seen.add(cf32)`.
            seen.add(cf32)
    # Returns the computed value to the caller.
    return unique


# Defines the function main.
def main():
    # Executes the statement `parser = argparse.ArgumentParser(description='Benchmark C++, Python, and optional GNU Radio receivers')`.
    parser = argparse.ArgumentParser(description='Benchmark C++, Python, and optional GNU Radio receivers')
    # Configures the argument parser for the CLI.
    parser.add_argument('--truth', default=str(DEFAULT_TRUTH), help='JSON truth file to seed vector list')
    # Configures the argument parser for the CLI.
    parser.add_argument('--roots', nargs='*', help='Additional vector directories (relative to repo root)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--runs', type=int, default=3, help='Number of repetitions per receiver (default 3)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--no-fast', action='store_true', help='Disable --fast flag for Python receiver')
    # Configures the argument parser for the CLI.
    parser.add_argument('--limit', type=int, help='Limit number of vectors (for quick testing)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--output', help='Optional JSON output path for benchmarking results')
    # Configures the argument parser for the CLI.
    parser.add_argument('--with-gr', action='store_true', help='Include GNU Radio offline decoder in benchmark')
    # Configures the argument parser for the CLI.
    parser.add_argument('--gr-python', type=str, default=None, help='Python interpreter for GNU Radio (optional)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--gr-conda-env', type=str, default=None, help='Conda env name to activate for GR (alternative to --gr-python)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--cpp-skip-syncword', action='store_true', help='Always pass --skip-syncword to C++ decoder')
    # Configures the argument parser for the CLI.
    parser.add_argument('--cpp-fallback-skip-syncword', action='store_true', help='Retry C++ with --skip-syncword if first attempt fails')
    # Executes the statement `args = parser.parse_args()`.
    args = parser.parse_args()

    # Executes the statement `truth_path = Path(args.truth)`.
    truth_path = Path(args.truth)
    # Executes the statement `roots = [Path(r) for r in args.roots] if args.roots else None`.
    roots = [Path(r) for r in args.roots] if args.roots else None
    # Executes the statement `vectors = collect_vectors(truth_path, roots)`.
    vectors = collect_vectors(truth_path, roots)
    # Begins a conditional branch to check a condition.
    if args.limit:
        # Executes the statement `vectors = vectors[:args.limit]`.
        vectors = vectors[:args.limit]

    # Executes the statement `runs = max(1, args.runs)`.
    runs = max(1, args.runs)
    # Executes the statement `fast = not args.no_fast`.
    fast = not args.no_fast

    # Executes the statement `results: Dict[str, Any] = {}`.
    results: Dict[str, Any] = {}
    # Executes the statement `total_cpp = 0.0`.
    total_cpp = 0.0
    # Executes the statement `total_py = 0.0`.
    total_py = 0.0
    # Executes the statement `total_gr = 0.0`.
    total_gr = 0.0
    # Executes the statement `count_cpp = 0`.
    count_cpp = 0
    # Executes the statement `count_py = 0`.
    count_py = 0
    # Executes the statement `count_gr = 0`.
    count_gr = 0

    # Starts a loop iterating over a sequence.
    for cf32 in vectors:
        # Executes the statement `rel = cf32.relative_to(ROOT)`.
        rel = cf32.relative_to(ROOT)
        # Executes the statement `meta = load_meta(cf32)`.
        meta = load_meta(cf32)

        # Executes the statement `cpp_res = time_command(make_cpp_cmd(cf32, meta, skip_syncword=args.cpp_skip_syncword), runs)`.
        cpp_res = time_command(make_cpp_cmd(cf32, meta, skip_syncword=args.cpp_skip_syncword), runs)
        # Executes the statement `py_res = time_command(make_py_cmd(cf32, fast=fast), runs)`.
        py_res = time_command(make_py_cmd(cf32, fast=fast), runs)
        # Executes the statement `gr_res: Dict[str, Any] | None = None`.
        gr_res: Dict[str, Any] | None = None
        # Begins a conditional branch to check a condition.
        if args.with_gr and GR_SCRIPT.exists():
            # Executes the statement `gr_res = time_command(make_gr_cmd(cf32, meta, args.gr_python, args.gr_conda_env), runs)`.
            gr_res = time_command(make_gr_cmd(cf32, meta, args.gr_python, args.gr_conda_env), runs)

        # Optional fallback for C++: retry with --skip-syncword when first attempt fails
        # Executes the statement `cpp_used_skip = args.cpp_skip_syncword`.
        cpp_used_skip = args.cpp_skip_syncword
        # Begins a conditional branch to check a condition.
        if cpp_res['status'] != 'ok' and args.cpp_fallback_skip_syncword and not args.cpp_skip_syncword:
            # Executes the statement `cpp_res_fb = time_command(make_cpp_cmd(cf32, meta, skip_syncword=True), runs)`.
            cpp_res_fb = time_command(make_cpp_cmd(cf32, meta, skip_syncword=True), runs)
            # Begins a conditional branch to check a condition.
            if cpp_res_fb['status'] == 'ok':
                # Executes the statement `cpp_res = cpp_res_fb`.
                cpp_res = cpp_res_fb
                # Executes the statement `cpp_used_skip = True`.
                cpp_used_skip = True

        # Executes the statement `entry: Dict[str, Any] = {`.
        entry: Dict[str, Any] = {
            # Executes the statement `'sf': meta.get('sf'),`.
            'sf': meta.get('sf'),
            # Executes the statement `'bw': meta.get('bw'),`.
            'bw': meta.get('bw'),
            # Executes the statement `'cr': meta.get('cr'),`.
            'cr': meta.get('cr'),
            # Executes the statement `'impl_header': bool(meta.get('impl_header', False)),`.
            'impl_header': bool(meta.get('impl_header', False)),
            # Executes the statement `'ldro_mode': meta.get('ldro_mode', 0),`.
            'ldro_mode': meta.get('ldro_mode', 0),
            # Executes the statement `'payload_len': meta.get('payload_len'),`.
            'payload_len': meta.get('payload_len'),
        # Closes the previously opened dictionary or set literal.
        }

        # Begins a conditional branch to check a condition.
        if cpp_res['status'] == 'ok':
            # Executes the statement `entry['cpp_avg_sec'] = cpp_res['avg_sec']`.
            entry['cpp_avg_sec'] = cpp_res['avg_sec']
            # Executes the statement `entry['cpp_std_sec'] = cpp_res['std_sec']`.
            entry['cpp_std_sec'] = cpp_res['std_sec']
            # Executes the statement `entry['cpp_avg_user_sec'] = cpp_res.get('avg_user_sec')`.
            entry['cpp_avg_user_sec'] = cpp_res.get('avg_user_sec')
            # Executes the statement `entry['cpp_avg_sys_sec'] = cpp_res.get('avg_sys_sec')`.
            entry['cpp_avg_sys_sec'] = cpp_res.get('avg_sys_sec')
            # Executes the statement `entry['cpp_avg_maxrss_kb'] = cpp_res.get('avg_maxrss_kb')`.
            entry['cpp_avg_maxrss_kb'] = cpp_res.get('avg_maxrss_kb')
            # Executes the statement `entry['cpp_payload_hex'] = parse_payload_hex(cpp_res['stdout'])`.
            entry['cpp_payload_hex'] = parse_payload_hex(cpp_res['stdout'])
            # Executes the statement `entry['cpp_used_skip_syncword'] = cpp_used_skip`.
            entry['cpp_used_skip_syncword'] = cpp_used_skip
            # Executes the statement `total_cpp += cpp_res['avg_sec']`.
            total_cpp += cpp_res['avg_sec']
            # Executes the statement `count_cpp += 1`.
            count_cpp += 1
        # Provides the fallback branch when previous conditions fail.
        else:
            # Executes the statement `entry['cpp_error'] = cpp_res`.
            entry['cpp_error'] = cpp_res

        # Begins a conditional branch to check a condition.
        if py_res['status'] == 'ok':
            # Executes the statement `entry['py_avg_sec'] = py_res['avg_sec']`.
            entry['py_avg_sec'] = py_res['avg_sec']
            # Executes the statement `entry['py_std_sec'] = py_res['std_sec']`.
            entry['py_std_sec'] = py_res['std_sec']
            # Executes the statement `entry['py_avg_user_sec'] = py_res.get('avg_user_sec')`.
            entry['py_avg_user_sec'] = py_res.get('avg_user_sec')
            # Executes the statement `entry['py_avg_sys_sec'] = py_res.get('avg_sys_sec')`.
            entry['py_avg_sys_sec'] = py_res.get('avg_sys_sec')
            # Executes the statement `entry['py_avg_maxrss_kb'] = py_res.get('avg_maxrss_kb')`.
            entry['py_avg_maxrss_kb'] = py_res.get('avg_maxrss_kb')
            # Executes the statement `entry['python_payload_hex'] = ''`.
            entry['python_payload_hex'] = ''
            # Executes the statement `total_py += py_res['avg_sec']`.
            total_py += py_res['avg_sec']
            # Executes the statement `count_py += 1`.
            count_py += 1
        # Provides the fallback branch when previous conditions fail.
        else:
            # Executes the statement `entry['py_error'] = py_res`.
            entry['py_error'] = py_res

        # Begins a conditional branch to check a condition.
        if gr_res is not None:
            # Begins a conditional branch to check a condition.
            if gr_res['status'] == 'ok':
                # Executes the statement `entry['gr_avg_sec'] = gr_res['avg_sec']`.
                entry['gr_avg_sec'] = gr_res['avg_sec']
                # Executes the statement `entry['gr_std_sec'] = gr_res['std_sec']`.
                entry['gr_std_sec'] = gr_res['std_sec']
                # Executes the statement `entry['gr_avg_user_sec'] = gr_res.get('avg_user_sec')`.
                entry['gr_avg_user_sec'] = gr_res.get('avg_user_sec')
                # Executes the statement `entry['gr_avg_sys_sec'] = gr_res.get('avg_sys_sec')`.
                entry['gr_avg_sys_sec'] = gr_res.get('avg_sys_sec')
                # Executes the statement `entry['gr_avg_maxrss_kb'] = gr_res.get('avg_maxrss_kb')`.
                entry['gr_avg_maxrss_kb'] = gr_res.get('avg_maxrss_kb')
                # Extract hex from stdout of GR script
                # Executes the statement `gr_hex = ''`.
                gr_hex = ''
                # Starts a loop iterating over a sequence.
                for line in (gr_res.get('stdout') or '').splitlines():
                    # Executes the statement `line = line.strip()`.
                    line = line.strip()
                    # Begins a conditional branch to check a condition.
                    if line.lower().startswith('hex:'):
                        # Executes the statement `gr_hex = line.split(':', 1)[1].strip().replace(' ', '').lower()`.
                        gr_hex = line.split(':', 1)[1].strip().replace(' ', '').lower()
                # Begins a conditional branch to check a condition.
                if gr_hex:
                    # Executes the statement `entry['gr_payload_hex'] = gr_hex`.
                    entry['gr_payload_hex'] = gr_hex
                # Executes the statement `total_gr += gr_res['avg_sec'] or 0.0`.
                total_gr += gr_res['avg_sec'] or 0.0
                # Executes the statement `count_gr += 1`.
                count_gr += 1
            # Provides the fallback branch when previous conditions fail.
            else:
                # Executes the statement `entry['gr_error'] = gr_res`.
                entry['gr_error'] = gr_res

        # Executes the statement `results[str(rel)] = entry`.
        results[str(rel)] = entry

    # Executes the statement `summary = {`.
    summary = {
        # Executes the statement `'vector_count': len(results),`.
        'vector_count': len(results),
        # Executes the statement `'cpp_avg_total': (total_cpp / count_cpp) if count_cpp else None,`.
        'cpp_avg_total': (total_cpp / count_cpp) if count_cpp else None,
        # Executes the statement `'py_avg_total': (total_py / count_py) if count_py else None,`.
        'py_avg_total': (total_py / count_py) if count_py else None,
        # Executes the statement `'gr_avg_total': (total_gr / count_gr) if count_gr else None,`.
        'gr_avg_total': (total_gr / count_gr) if count_gr else None,
        # Executes the statement `'fast_mode': fast,`.
        'fast_mode': fast,
        # Executes the statement `'runs_per_vector': runs,`.
        'runs_per_vector': runs,
        # Executes the statement `'entries': results,`.
        'entries': results,
    # Closes the previously opened dictionary or set literal.
    }

    # Begins a conditional branch to check a condition.
    if args.output:
        # Executes the statement `out_path = (ROOT / args.output).resolve()`.
        out_path = (ROOT / args.output).resolve()
        # Executes the statement `out_path.parent.mkdir(parents=True, exist_ok=True)`.
        out_path.parent.mkdir(parents=True, exist_ok=True)
        # Executes the statement `out_path.write_text(json.dumps(summary, indent=2))`.
        out_path.write_text(json.dumps(summary, indent=2))
        # Outputs diagnostic or user-facing text.
        print(f'Wrote results to {out_path}')
    # Provides the fallback branch when previous conditions fail.
    else:
        # Outputs diagnostic or user-facing text.
        print(json.dumps(summary, indent=2))


# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
