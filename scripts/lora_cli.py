#!/usr/bin/env python3
"""
Universal CLI for LoRa Lite PHY (relocated under scripts/)

Subcommands:
  - decode: Decode a single IQ file with the Python receiver
  - batch:  Batch process files/directories (wraps scripts.batch_lora_decoder)
  - test:   Run the receiver test suite (wraps scripts.lora_test_suite)
  - demo:   Show final system demos (wraps scripts.final_system_demo/celebration_demo)
  - analyze: Handy analysis helpers from analysis/* modules

Run via:
  python -m scripts.lora_cli --help
"""

import argparse
import json
import sys
from pathlib import Path

# Ensure project root is importable (for complete_lora_receiver and analysis/*)
ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from receiver.receiver import LoRaReceiver

def cmd_decode(args: argparse.Namespace) -> int:
    receiver = LoRaReceiver(
        sf=args.sf,
        bw=args.bw,
        cr=args.cr,
        has_crc=args.crc,
        impl_head=args.impl_head,
        ldro_mode=args.ldro_mode,
        samp_rate=args.samp_rate,
        sync_words=[int(x, 16) if x.startswith("0x") else int(x) for x in args.sync_words.split(',')]
    )
    result = receiver.decode_file(args.input)
    if args.output:
        Path(args.output).parent.mkdir(parents=True, exist_ok=True)
        with open(args.output, 'w') as f:
            json.dump(result, f, indent=2)
    if args.verbose or not args.output:
        print(json.dumps(result, indent=2))
    return 0 if result.get('status') != 'error' else 1

def cmd_batch(args: argparse.Namespace) -> int:
    # Defer implementation to existing script within this package
    from .batch_lora_decoder import main as batch_main
    argv = [
        'batch_lora_decoder.py',
        args.input,
    ]
    if args.sf: argv += ['--sf', str(args.sf)]
    if args.bw: argv += ['--bw', str(args.bw)]
    if args.cr: argv += ['--cr', str(args.cr)]
    if args.crc is True: argv += ['--crc']
    if args.crc is False: argv += ['--no-crc']
    if args.impl_head: argv += ['--impl-head']
    if args.samp_rate: argv += ['--samp-rate', str(args.samp_rate)]
    if args.output_dir: argv += ['--output-dir', args.output_dir]
    if args.summary_file: argv += ['--summary-file', args.summary_file]
    if args.verbose: argv += ['--verbose']
    old = sys.argv
    try:
        sys.argv = argv
        return batch_main()
    finally:
        sys.argv = old

def cmd_test(args: argparse.Namespace) -> int:
    from .lora_test_suite import main as tests_main
    argv = ['lora_test_suite.py']
    if args.quick_test: argv += ['--quick-test']
    if args.test_vectors_dir: argv += ['--test-vectors-dir', args.test_vectors_dir]
    if args.output_report: argv += ['--output-report', args.output_report]
    old = sys.argv
    try:
        sys.argv = argv
        return tests_main()
    finally:
        sys.argv = old

def cmd_demo(args: argparse.Namespace) -> int:
    if args.mode == 'final':
        from .final_system_demo import (
            demonstrate_complete_system,
            show_technical_specs,
            show_breakthrough_methods,
            show_validation_results,
            final_celebration,
        )
        demonstrate_complete_system()
        show_technical_specs()
        show_breakthrough_methods()
        show_validation_results()
        final_celebration()
    else:
        from .celebration_demo import celebration_demo, final_celebration
        celebration_demo()
        final_celebration()
    return 0

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
    pd = sub.add_parser('decode', help='Decode a single IQ file')
    pd.add_argument('input', help='Input CF32 IQ file path')
    pd.add_argument('--sf', type=int, default=7, choices=range(7, 13))
    pd.add_argument('--bw', type=int, default=125000, choices=[125000, 250000, 500000])
    pd.add_argument('--cr', type=int, default=2, choices=range(1, 5))
    pd.add_argument('--crc', dest='crc', action='store_true', default=True)
    pd.add_argument('--no-crc', dest='crc', action='store_false')
    pd.add_argument('--impl-head', action='store_true')
    pd.add_argument('--ldro-mode', type=int, default=0, choices=[0,1,2])
    pd.add_argument('--samp-rate', type=int, default=500000)
    pd.add_argument('--sync-words', type=str, default='0x12')
    pd.add_argument('--output', '-o')
    pd.add_argument('--verbose', '-v', action='store_true')
    pd.set_defaults(func=cmd_decode)

    # batch
    pb = sub.add_parser('batch', help='Batch process files or directories')
    pb.add_argument('input', help='Input file or directory')
    pb.add_argument('--sf', type=int, choices=range(7, 13))
    pb.add_argument('--bw', type=int, choices=[125000, 250000, 500000])
    pb.add_argument('--cr', type=int, choices=range(1, 5))
    pb.add_argument('--crc', dest='crc', action='store_true')
    pb.add_argument('--no-crc', dest='crc', action='store_false')
    pb.add_argument('--impl-head', action='store_true')
    pb.add_argument('--samp-rate', type=int, default=500000)
    pb.add_argument('--output-dir', '-o')
    pb.add_argument('--summary-file', default='batch_summary.json')
    pb.add_argument('--verbose', '-v', action='store_true')
    pb.set_defaults(func=cmd_batch)

    # test
    pt = sub.add_parser('test', help='Run the receiver test suite')
    pt.add_argument('--quick-test', action='store_true')
    pt.add_argument('--test-vectors-dir', default='vectors')
    pt.add_argument('--output-report', default='test_report.json')
    pt.set_defaults(func=cmd_test)

    # demo
    pde = sub.add_parser('demo', help='Run final system demos')
    pde.add_argument('--mode', choices=['final', 'celebration'], default='final')
    pde.set_defaults(func=cmd_demo)

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
