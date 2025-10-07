#!/usr/bin/env python3
"""Re-run failed vectors from a results JSON and dump header IQ slices.

Reads results/streaming_compat_results.json (or a provided path), filters to
failed cpp_stream entries, and re-runs decode_cli in streaming mode with:
  - --hdr-cfo-sweep
  - --dump-header-iq results/hdr_slices/<stem>_header.cf32
Optionally limit to N vectors and restrict by prefix.
"""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Dict

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_RESULTS = ROOT / 'results' / 'streaming_compat_results.json'
CPP_CANDIDATES = [
    ROOT / 'cpp_receiver/build/decode_cli',
    ROOT / 'cpp_receiver/build/Release/decode_cli',
]

def resolve_cpp_binary() -> Path | None:
    for p in CPP_CANDIDATES:
        if p.exists():
            return p
    return None


def main() -> None:
    ap = argparse.ArgumentParser(description='Dump header IQ slices for failed streaming vectors')
    ap.add_argument('--results', type=Path, default=DEFAULT_RESULTS)
    ap.add_argument('--prefix', type=str, default='')
    ap.add_argument('--limit', type=int, default=25)
    ap.add_argument('--out-dir', type=Path, default=ROOT / 'results' / 'hdr_slices')
    ap.add_argument('--hdr-cfo-range', type=float, default=200.0)
    ap.add_argument('--hdr-cfo-step', type=float, default=25.0)
    ap.add_argument('--slice-payload-syms', type=int, default=96, help='Extra payload symbols to include in slice')
    ap.add_argument('--slice-always', action='store_true', help='Dump slice even when header decode fails')
    args = ap.parse_args()

    binary = resolve_cpp_binary()
    if not binary:
        raise SystemExit('decode_cli not found; build cpp_receiver first')

    data = json.loads(args.results.read_text())
    failures = []
    for item in data.get('results', []):
        vec = item.get('vector')
        if not vec:
            continue
        if args.prefix and not str(vec).startswith(args.prefix):
            continue
        cpp = item.get('cpp_stream', {})
        if cpp.get('status') == 'success':
            continue
        meta: Dict = item.get('metadata', {})
        failures.append((vec, meta))

    if not failures:
        print('No failures to re-run')
        return

    failures = failures[: args.limit]
    args.out_dir.mkdir(parents=True, exist_ok=True)

    for idx, (vec, meta) in enumerate(failures, 1):
        vec_path = Path(vec)
        fs = int(meta.get('samp_rate') or meta.get('sample_rate'))
        ldro = int(meta.get('ldro_mode', 0))
        cmd = [
            str(binary),
            '--sf', str(meta['sf']),
            '--bw', str(meta['bw']),
            '--fs', str(fs),
            '--ldro', '1' if ldro else '0',
            '--sync-word', str(meta.get('sync_word', 0x12)),
            '--streaming',
            '--hdr-cfo-sweep',
            '--hdr-cfo-range', str(args.hdr_cfo_range),
            '--hdr-cfo-step', str(args.hdr_cfo_step),
            '--dump-header-iq', str(args.out_dir / f"{vec_path.stem}_header.cf32"),
            '--dump-header-iq-payload-syms', str(args.slice_payload_syms),
            str(vec_path),
        ]
        if args.slice_always:
            cmd.append('--dump-header-iq-always')
        implicit = bool(meta.get('impl_header') or meta.get('implicit_header'))
        if implicit:
            payload_len = int(meta.get('payload_len') or meta.get('payload_length') or 0)
            cr = int(meta.get('cr', 1))
            cmd.extend(['--implicit-header', '--payload-len', str(payload_len), '--cr', str(cr)])
            if meta.get('crc', True):
                cmd.append('--has-crc')
            else:
                cmd.append('--no-crc')
        print(f"[{idx}/{len(failures)}] dumping header slice for {vec_path.name}")
        try:
            subprocess.run(cmd, check=False, capture_output=True, text=True, timeout=90)
        except subprocess.TimeoutExpired:
            print(f"Timeout on {vec_path}")


if __name__ == '__main__':
    main()
