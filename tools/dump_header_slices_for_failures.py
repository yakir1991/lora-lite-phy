#!/usr/bin/env python3
# This file provides the 'dump header slices for failures' functionality for the LoRa Lite PHY toolkit.
"""Re-run failed vectors from a results JSON and dump header IQ slices.

Reads results/streaming_compat_results.json (or a provided path), filters to
failed cpp_stream entries, and re-runs decode_cli in streaming mode with:
  - --hdr-cfo-sweep
  - --dump-header-iq results/hdr_slices/<stem>_header.cf32
Optionally limit to N vectors and restrict by prefix.
"""

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
# Imports specific objects with 'from typing import Dict'.
from typing import Dict

# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `DEFAULT_RESULTS = ROOT / 'results' / 'streaming_compat_results.json'`.
DEFAULT_RESULTS = ROOT / 'results' / 'streaming_compat_results.json'
# Executes the statement `CPP_CANDIDATES = [`.
CPP_CANDIDATES = [
    # Executes the statement `ROOT / 'cpp_receiver/build/decode_cli',`.
    ROOT / 'cpp_receiver/build/decode_cli',
    # Executes the statement `ROOT / 'cpp_receiver/build/Release/decode_cli',`.
    ROOT / 'cpp_receiver/build/Release/decode_cli',
# Closes the previously opened list indexing or literal.
]

# Defines the function resolve_cpp_binary.
def resolve_cpp_binary() -> Path | None:
    # Starts a loop iterating over a sequence.
    for p in CPP_CANDIDATES:
        # Begins a conditional branch to check a condition.
        if p.exists():
            # Returns the computed value to the caller.
            return p
    # Returns the computed value to the caller.
    return None


# Defines the function main.
def main() -> None:
    # Executes the statement `ap = argparse.ArgumentParser(description='Dump header IQ slices for failed streaming vectors')`.
    ap = argparse.ArgumentParser(description='Dump header IQ slices for failed streaming vectors')
    # Executes the statement `ap.add_argument('--results', type=Path, default=DEFAULT_RESULTS)`.
    ap.add_argument('--results', type=Path, default=DEFAULT_RESULTS)
    # Executes the statement `ap.add_argument('--prefix', type=str, default='')`.
    ap.add_argument('--prefix', type=str, default='')
    # Executes the statement `ap.add_argument('--limit', type=int, default=25)`.
    ap.add_argument('--limit', type=int, default=25)
    # Executes the statement `ap.add_argument('--out-dir', type=Path, default=ROOT / 'results' / 'hdr_slices')`.
    ap.add_argument('--out-dir', type=Path, default=ROOT / 'results' / 'hdr_slices')
    # Executes the statement `ap.add_argument('--hdr-cfo-range', type=float, default=200.0)`.
    ap.add_argument('--hdr-cfo-range', type=float, default=200.0)
    # Executes the statement `ap.add_argument('--hdr-cfo-step', type=float, default=25.0)`.
    ap.add_argument('--hdr-cfo-step', type=float, default=25.0)
    # Executes the statement `ap.add_argument('--slice-payload-syms', type=int, default=96, help='Extra payload symbols to include in slice')`.
    ap.add_argument('--slice-payload-syms', type=int, default=96, help='Extra payload symbols to include in slice')
    # Executes the statement `ap.add_argument('--slice-always', action='store_true', help='Dump slice even when header decode fails')`.
    ap.add_argument('--slice-always', action='store_true', help='Dump slice even when header decode fails')
    # Executes the statement `args = ap.parse_args()`.
    args = ap.parse_args()

    # Executes the statement `binary = resolve_cpp_binary()`.
    binary = resolve_cpp_binary()
    # Begins a conditional branch to check a condition.
    if not binary:
        # Raises an exception to signal an error.
        raise SystemExit('decode_cli not found; build cpp_receiver first')

    # Executes the statement `data = json.loads(args.results.read_text())`.
    data = json.loads(args.results.read_text())
    # Executes the statement `failures = []`.
    failures = []
    # Starts a loop iterating over a sequence.
    for item in data.get('results', []):
        # Executes the statement `vec = item.get('vector')`.
        vec = item.get('vector')
        # Begins a conditional branch to check a condition.
        if not vec:
            # Skips to the next iteration of the loop.
            continue
        # Begins a conditional branch to check a condition.
        if args.prefix and not str(vec).startswith(args.prefix):
            # Skips to the next iteration of the loop.
            continue
        # Executes the statement `cpp = item.get('cpp_stream', {})`.
        cpp = item.get('cpp_stream', {})
        # Begins a conditional branch to check a condition.
        if cpp.get('status') == 'success':
            # Skips to the next iteration of the loop.
            continue
        # Executes the statement `meta: Dict = item.get('metadata', {})`.
        meta: Dict = item.get('metadata', {})
        # Executes the statement `failures.append((vec, meta))`.
        failures.append((vec, meta))

    # Begins a conditional branch to check a condition.
    if not failures:
        # Outputs diagnostic or user-facing text.
        print('No failures to re-run')
        # Returns control to the caller.
        return

    # Executes the statement `failures = failures[: args.limit]`.
    failures = failures[: args.limit]
    # Accesses a parsed command-line argument.
    args.out_dir.mkdir(parents=True, exist_ok=True)

    # Starts a loop iterating over a sequence.
    for idx, (vec, meta) in enumerate(failures, 1):
        # Executes the statement `vec_path = Path(vec)`.
        vec_path = Path(vec)
        # Executes the statement `fs = int(meta.get('samp_rate') or meta.get('sample_rate'))`.
        fs = int(meta.get('samp_rate') or meta.get('sample_rate'))
        # Executes the statement `ldro = int(meta.get('ldro_mode', 0))`.
        ldro = int(meta.get('ldro_mode', 0))
        # Executes the statement `cmd = [`.
        cmd = [
            # Executes the statement `str(binary),`.
            str(binary),
            # Executes the statement `'--sf', str(meta['sf']),`.
            '--sf', str(meta['sf']),
            # Executes the statement `'--bw', str(meta['bw']),`.
            '--bw', str(meta['bw']),
            # Executes the statement `'--fs', str(fs),`.
            '--fs', str(fs),
            # Executes the statement `'--ldro', '1' if ldro else '0',`.
            '--ldro', '1' if ldro else '0',
            # Executes the statement `'--sync-word', str(meta.get('sync_word', 0x12)),`.
            '--sync-word', str(meta.get('sync_word', 0x12)),
            # Executes the statement `'--streaming',`.
            '--streaming',
            # Executes the statement `'--hdr-cfo-sweep',`.
            '--hdr-cfo-sweep',
            # Executes the statement `'--hdr-cfo-range', str(args.hdr_cfo_range),`.
            '--hdr-cfo-range', str(args.hdr_cfo_range),
            # Executes the statement `'--hdr-cfo-step', str(args.hdr_cfo_step),`.
            '--hdr-cfo-step', str(args.hdr_cfo_step),
            # Executes the statement `'--dump-header-iq', str(args.out_dir / f"{vec_path.stem}_header.cf32"),`.
            '--dump-header-iq', str(args.out_dir / f"{vec_path.stem}_header.cf32"),
            # Executes the statement `'--dump-header-iq-payload-syms', str(args.slice_payload_syms),`.
            '--dump-header-iq-payload-syms', str(args.slice_payload_syms),
            # Executes the statement `str(vec_path),`.
            str(vec_path),
        # Closes the previously opened list indexing or literal.
        ]
        # Begins a conditional branch to check a condition.
        if args.slice_always:
            # Executes the statement `cmd.append('--dump-header-iq-always')`.
            cmd.append('--dump-header-iq-always')
        # Executes the statement `implicit = bool(meta.get('impl_header') or meta.get('implicit_header'))`.
        implicit = bool(meta.get('impl_header') or meta.get('implicit_header'))
        # Begins a conditional branch to check a condition.
        if implicit:
            # Executes the statement `payload_len = int(meta.get('payload_len') or meta.get('payload_length') or 0)`.
            payload_len = int(meta.get('payload_len') or meta.get('payload_length') or 0)
            # Executes the statement `cr = int(meta.get('cr', 1))`.
            cr = int(meta.get('cr', 1))
            # Executes the statement `cmd.extend(['--implicit-header', '--payload-len', str(payload_len), '--cr', str(cr)])`.
            cmd.extend(['--implicit-header', '--payload-len', str(payload_len), '--cr', str(cr)])
            # Begins a conditional branch to check a condition.
            if meta.get('crc', True):
                # Executes the statement `cmd.append('--has-crc')`.
                cmd.append('--has-crc')
            # Provides the fallback branch when previous conditions fail.
            else:
                # Executes the statement `cmd.append('--no-crc')`.
                cmd.append('--no-crc')
        # Outputs diagnostic or user-facing text.
        print(f"[{idx}/{len(failures)}] dumping header slice for {vec_path.name}")
        # Begins a block that monitors for exceptions.
        try:
            # Executes the statement `subprocess.run(cmd, check=False, capture_output=True, text=True, timeout=90)`.
            subprocess.run(cmd, check=False, capture_output=True, text=True, timeout=90)
        # Handles a specific exception from the try block.
        except subprocess.TimeoutExpired:
            # Outputs diagnostic or user-facing text.
            print(f"Timeout on {vec_path}")


# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
