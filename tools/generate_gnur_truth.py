#!/usr/bin/env python3
"""Run GNU Radio offline decoder on vectors to collect payload truth."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Dict, Any, List, Tuple

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'external' / 'gr_lora_sdr' / 'scripts' / 'decode_offline_recording.py'
DEFAULT_ROOTS = [
    Path('golden_vectors/new_batch'),
    Path('golden_vectors/extended_batch'),
    Path('golden_vectors/extended_batch_crc_off'),
    Path('golden_vectors/extended_batch_impl'),
    Path('golden_vectors/demo'),
    Path('golden_vectors/demo_batch'),
    Path('golden_vectors/custom'),
]


def load_meta(cf32: Path) -> Dict[str, Any]:
    meta_path = cf32.with_suffix('.json')
    if not meta_path.exists():
        raise FileNotFoundError(f'Metadata JSON missing for {cf32}')
    return json.loads(meta_path.read_text())


def gnur_cmd(cf32: Path, meta: Dict[str, Any]) -> List[str]:
    samp_rate = int(meta.get('samp_rate') or meta.get('sample_rate') or meta.get('fs') or 0)
    if samp_rate <= 0:
        samp_rate = int(meta.get('bw', 0))
    cmd = [
        'conda', 'run', '-n', 'gr310', 'python', str(SCRIPT), str(cf32),
        '--sf', str(meta.get('sf')),
        '--bw', str(meta.get('bw')),
        '--samp-rate', str(samp_rate),
        '--cr', str(meta.get('cr', 1)),
        '--pay-len', str(meta.get('payload_len', 255)),
        '--ldro-mode', str(meta.get('ldro_mode', 2)),
        '--sync-word', hex(int(meta.get('sync_word', 0x12))),
    ]
    if meta.get('crc', True):
        cmd.append('--has-crc')
    else:
        cmd.append('--no-crc')
    if meta.get('impl_header', False):
        cmd.append('--impl-header')
    else:
        cmd.append('--explicit-header')
    return cmd


def run_gnur(cf32: Path, meta: Dict[str, Any], timeout: int) -> Dict[str, Any]:
    if not SCRIPT.exists():
        return {'status': 'error', 'stderr': f'script {SCRIPT} missing'}
    cmd = gnur_cmd(cf32, meta)
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        stderr = exc.stderr.decode() if isinstance(exc.stderr, bytes) else (exc.stderr or '')
        return {'status': 'timeout', 'stderr': stderr}
    if proc.returncode != 0:
        return {
            'status': 'error',
            'returncode': proc.returncode,
            'stdout': proc.stdout,
            'stderr': proc.stderr,
        }
    payloads: List[str] = []
    crc: List[str] = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line.startswith('Frame '):
            if 'CRC' in line:
                crc_state = line.split('CRC', 1)[1].strip()
                crc.append(crc_state)
        if line.startswith('Hex:'):
            hex_tokens = line.split(':', 1)[1].strip().split()
            payloads.append(''.join(hex_tokens).lower())
    if not payloads:
        return {
            'status': 'ok',
            'payloads': [],
            'stdout': proc.stdout,
        }
    return {
        'status': 'ok',
        'payloads': payloads,
        'crc': crc,
        'stdout': proc.stdout,
    }


def collect_vectors(roots: List[Path]) -> List[Path]:
    vectors: List[Path] = []
    for root in roots:
        base = (ROOT / root).resolve()
        if not base.exists():
            continue
        vectors.extend(sorted(base.glob('*.cf32')))
    return vectors


def main():
    parser = argparse.ArgumentParser(description='Generate GNU Radio payload truth for vectors')
    parser.add_argument('--roots', nargs='*', default=None,
                        help='Vector directories (relative to repo root); default covers golden sets')
    parser.add_argument('--output', default='results/gnur_truth.json', help='Output JSON file')
    parser.add_argument('--timeout', type=int, default=180, help='Timeout per vector (seconds)')
    parser.add_argument('--limit', type=int, help='Limit number of vectors for quick tests')
    args = parser.parse_args()

    roots = [Path(r) for r in args.roots] if args.roots else DEFAULT_ROOTS
    vectors = collect_vectors(roots)
    if args.limit:
        vectors = vectors[:args.limit]

    results: Dict[str, Any] = {}
    out_path = (ROOT / args.output).resolve()
    if out_path.exists():
        results = json.loads(out_path.read_text())

    for cf32 in vectors:
        rel = str(cf32.relative_to(ROOT))
        meta = load_meta(cf32)
        res = run_gnur(cf32, meta, timeout=args.timeout)
        results[rel] = {
            'sf': meta.get('sf'),
            'bw': meta.get('bw'),
            'cr': meta.get('cr'),
            'ldro_mode': meta.get('ldro_mode'),
            'impl_header': bool(meta.get('impl_header', False)),
            **res,
        }
        print(f"[gnur] {rel}: {res.get('status')}")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(results, indent=2))
    print(f"Wrote {len(results)} entries -> {out_path}")


if __name__ == '__main__':
    main()
