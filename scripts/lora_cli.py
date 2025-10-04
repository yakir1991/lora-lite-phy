#!/usr/bin/env python3
"""
Universal CLI for LoRa Lite PHY (relocated under scripts/)

Subcommands:
  - decode: Decode a single IQ file with the GNU Radio Python reference
  - batch:  Batch process files/directories (wraps scripts.sdr_lora_batch_decode)
  - test:   Run the receiver test suite (wraps scripts.lora_test_suite)
  - analyze: Handy analysis helpers from analysis/* modules

Run via:
  python -m scripts.lora_cli --help
"""

import argparse
import sys
from pathlib import Path

# Ensure project root is importable (for analysis modules etc.)
ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


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


def cmd_test(args: argparse.Namespace) -> int:
    from .lora_test_suite import main as tests_main
    argv = ['lora_test_suite.py']
    if args.quick_test:
        argv += ['--quick-test']
    if args.test_vectors_dir:
        argv += ['--test-vectors-dir', args.test_vectors_dir]
    if args.output_report:
        argv += ['--output-report', args.output_report]
    old = sys.argv
    try:
        sys.argv = argv
        return tests_main()
    finally:
        sys.argv = old


def cmd_analyze(args: argparse.Namespace) -> int:
    if args.which == 'symbols':
        from analysis.advanced_demod_analysis import deep_symbol_analysis, test_advanced_fft_methods
        deep_symbol_analysis()
        score, method = test_advanced_fft_methods()
        print(f"Best method: {method} score={score}/8")
        return 0
    if args.which == 'integrated':
        from analysis.integrated_receiver import (
            run_full_cpp_analysis,
            extract_sync_parameters,
            calculate_precise_symbol_position,
            test_advanced_demodulation,
        )
        cpp_results = run_full_cpp_analysis()
        sync = extract_sync_parameters(cpp_results)
        positions = calculate_precise_symbol_position(sync)
        if positions:
            best = test_advanced_demodulation(positions, sync)
            print(f"Best result: {best}")
        return 0
    print("Unknown analysis mode")
    return 1


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

    # test
    pt = sub.add_parser('test', help='Run the receiver test suite')
    pt.add_argument('--quick-test', action='store_true')
    pt.add_argument('--test-vectors-dir', default='vectors')
    pt.add_argument('--output-report', default='test_report.json')
    pt.set_defaults(func=cmd_test)

    # analyze
    pa = sub.add_parser('analyze', help='Run analysis helpers')
    pa.add_argument('--which', choices=['symbols', 'integrated'], default='symbols')
    pa.set_defaults(func=cmd_analyze)

    return p


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    raise SystemExit(main())
