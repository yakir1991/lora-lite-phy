#!/usr/bin/env python3
"""Universal CLI for LoRa Lite PHY.

This command consolidates the older one-off tools under ``tools/`` and
``scripts/`` into a single entry-point for decoding, truth-table generation,
receiver comparison, benchmarking, and compatibility sweeps.

Run via ``python -m scripts.lora_cli --help`` for a list of subcommands.
"""

import argparse
import json
import sys
from dataclasses import asdict
from pathlib import Path

# Ensure project root is importable (for analysis modules etc.)
ROOT = Path(__file__).resolve().parent.parent
PYTHON_MODULES = ROOT / "python_modules"
for candidate in (ROOT, PYTHON_MODULES):
    candidate_str = str(candidate)
    if candidate_str not in sys.path:
        sys.path.insert(0, candidate_str)

from python_modules import lora_universal_tool as universal


def cmd_decode(args: argparse.Namespace) -> int:
    from . import sdr_lora_cli

    argv = ['sdr_lora_cli.py', 'decode', args.input]
    if args.meta:
        argv += ['--meta', args.meta]
    if args.sf is not None:
        argv += ['--sf', str(args.sf)]
    if args.bw is not None:
        argv += ['--bw', str(args.bw)]
    if args.fs is not None:
        argv += ['--fs', str(args.fs)]
    if args.fast:
        argv.append('--fast')
    if args.verbose:
        argv.append('--verbose')
    old = sys.argv
    try:
        sys.argv = argv
        return sdr_lora_cli.main()
    finally:
        sys.argv = old


def cmd_batch(args: argparse.Namespace) -> int:
    from .sdr_lora_batch_decode import main as batch_main
    argv = ['sdr_lora_batch_decode.py']
    if args.roots:
        argv += ['--roots', *args.roots]
    if args.out:
        argv += ['--out', args.out]
    if args.fast:
        argv.append('--fast')
    old = sys.argv
    try:
        sys.argv = argv
        return batch_main()
    finally:
        sys.argv = old


def cmd_python_truth(args: argparse.Namespace) -> int:
    result = universal.run_python_truth(
        roots=args.roots,
        output=Path(args.output).resolve(),
        fast=not args.no_fast,
        keep_existing=not args.overwrite,
        timeout=args.timeout,
    )
    print(json.dumps(result, indent=2))
    return 0


def cmd_gnur_truth(args: argparse.Namespace) -> int:
    result = universal.run_gnur_truth(
        roots=args.roots,
        output=Path(args.output).resolve(),
        timeout=args.timeout,
        limit=args.limit,
        conda_env=args.conda_env,
    )
    print(json.dumps(result, indent=2))
    return 0


def cmd_compare(args: argparse.Namespace) -> int:
    summary = universal.run_receiver_comparison(
        roots=args.roots,
        timeout=args.timeout,
        fallback_skip=args.fallback_skip,
        limit=args.limit,
        output=Path(args.output).resolve(),
        python_truth=Path(args.python_truth).resolve(),
        gnur_truth=Path(args.gnur_truth).resolve(),
    )
    print(json.dumps(summary['counts'], indent=2))
    return 0


def cmd_benchmark(args: argparse.Namespace) -> int:
    vec = Path(args.vector).resolve()
    results = universal.benchmark_receivers(
        vec,
        runs=args.runs,
        include_python=not args.no_python,
        include_cpp=not args.no_cpp,
        include_gnur=args.with_gr,
        fast=not args.no_fast,
        conda_env=args.conda_env,
    )
    serialisable = {name: asdict(res) for name, res in results.items()}
    print(json.dumps(serialisable, indent=2))
    if args.output:
        out_path = Path(args.output).resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(serialisable, indent=2))
    return 0


def cmd_compat(args: argparse.Namespace) -> int:
    summary = universal.run_gnur_cpp_compat(
        Path(args.vectors_dir).resolve(),
        limit=args.limit,
        output=Path(args.output).resolve(),
    )
    print(json.dumps(summary['analysis'], indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description='LoRa Lite PHY - Universal CLI')
    sub = p.add_subparsers(dest='command', required=True)

    # decode
    pd = sub.add_parser('decode', help='Decode a single IQ file via external/sdr_lora')
    pd.add_argument('input', help='Input CF32 IQ file path')
    pd.add_argument('--meta', help='Optional sidecar JSON with LoRa parameters')
    pd.add_argument('--sf', type=int, help='Fallback spreading factor when metadata is missing')
    pd.add_argument('--bw', type=int, help='Fallback bandwidth (Hz) when metadata is missing')
    pd.add_argument('--fs', type=int, help='Fallback sample rate (Hz) when metadata is missing')
    pd.add_argument('--fast', action='store_true', help='Enable fast mode (reduced search)')
    pd.add_argument('--verbose', '-v', action='store_true')
    pd.set_defaults(func=cmd_decode)

    # batch
    pb = sub.add_parser('batch', help='Batch process vectors with the GNU Radio reference')
    pb.add_argument('--roots', nargs='*', help='Directories to scan for cf32+json pairs')
    pb.add_argument('--out', default='results/sdr_lora_batch.json', help='Where to write the JSON report')
    pb.add_argument('--fast', action='store_true', help='Enable fast mode (limited sweep)')
    pb.set_defaults(func=cmd_batch)

    # python-truth
    ppt = sub.add_parser('python-truth', help='Generate payload truth via scripts.sdr_lora_cli')
    ppt.add_argument('--roots', nargs='*', help='Vector roots (relative to repo root)')
    ppt.add_argument('--output', default=str(universal.PY_TRUTH_DEFAULT))
    ppt.add_argument('--no-fast', action='store_true', help='Disable fast mode when decoding')
    ppt.add_argument('--overwrite', action='store_true', help='Do not merge with existing JSON')
    ppt.add_argument('--timeout', type=int, default=180)
    ppt.set_defaults(func=cmd_python_truth)

    # gnur-truth
    pgt = sub.add_parser('gnur-truth', help='Generate payload truth via GNU Radio offline decoder')
    pgt.add_argument('--roots', nargs='*', help='Vector roots (relative to repo root)')
    pgt.add_argument('--output', default=str(universal.GNUR_TRUTH_DEFAULT))
    pgt.add_argument('--timeout', type=int, default=180)
    pgt.add_argument('--limit', type=int)
    pgt.add_argument('--conda-env', default='gr310')
    pgt.set_defaults(func=cmd_gnur_truth)

    # compare
    pc = sub.add_parser('compare', help='Compare C++, Python, and GNU Radio receivers')
    pc.add_argument('--roots', nargs='*', help='Vector roots (relative to repo root)')
    pc.add_argument('--timeout', type=int, default=45)
    pc.add_argument('--fallback-skip', action='store_true', help='Retry C++ with --skip-syncword on failure')
    pc.add_argument('--limit', type=int)
    pc.add_argument('--output', default='results/receiver_comparison.json')
    pc.add_argument('--python-truth', default=str(universal.PY_TRUTH_DEFAULT))
    pc.add_argument('--gnur-truth', default=str(universal.GNUR_TRUTH_DEFAULT))
    pc.set_defaults(func=cmd_compare)

    # benchmark
    pbm = sub.add_parser('benchmark', help='Benchmark receivers on a single vector')
    pbm.add_argument('vector', help='Path to .cf32 vector')
    pbm.add_argument('--runs', type=int, default=3)
    pbm.add_argument('--no-python', action='store_true')
    pbm.add_argument('--no-cpp', action='store_true')
    pbm.add_argument('--with-gr', action='store_true', help='Include GNU Radio offline decoder')
    pbm.add_argument('--no-fast', action='store_true', help='Disable fast mode for Python receiver')
    pbm.add_argument('--conda-env', default='gr310')
    pbm.add_argument('--output', help='Optional JSON output path')
    pbm.set_defaults(func=cmd_benchmark)

    # compat
    pcompat = sub.add_parser('compat', help='GNU Radio vs. C++ compatibility sweep')
    pcompat.add_argument('--vectors-dir', default='golden_vectors_demo_batch')
    pcompat.add_argument('--limit', type=int)
    pcompat.add_argument('--output', default='gnu_radio_compat_results.json')
    pcompat.set_defaults(func=cmd_compat)

    return p


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
