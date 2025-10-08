#!/usr/bin/env python3
# This file provides the 'lora cli' functionality for the LoRa Lite PHY toolkit.
"""
Universal CLI for LoRa Lite PHY (relocated under scripts/)

Subcommands:
  - decode: Decode a single IQ file with the GNU Radio Python reference
  - batch:  Batch process files/directories (wraps scripts.sdr_lora_batch_decode)

Run via:
  python -m scripts.lora_cli --help
"""

# Imports the module(s) argparse.
import argparse
# Imports the module(s) sys.
import sys
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path

# Ensure project root is importable (for analysis modules etc.)
# Executes the statement `ROOT = Path(__file__).resolve().parent.parent`.
ROOT = Path(__file__).resolve().parent.parent
# Executes the statement `PYTHON_MODULES = ROOT / "python_modules"`.
PYTHON_MODULES = ROOT / "python_modules"
# Starts a loop iterating over a sequence.
for candidate in (ROOT, PYTHON_MODULES):
    # Executes the statement `candidate_str = str(candidate)`.
    candidate_str = str(candidate)
    # Begins a conditional branch to check a condition.
    if candidate_str not in sys.path:
        # Executes the statement `sys.path.insert(0, candidate_str)`.
        sys.path.insert(0, candidate_str)


# Defines the function cmd_decode.
def cmd_decode(args: argparse.Namespace) -> int:
    # Imports specific objects with 'from . import sdr_lora_cli'.
    from . import sdr_lora_cli

    # Executes the statement `argv = ['sdr_lora_cli.py', 'decode', args.input]`.
    argv = ['sdr_lora_cli.py', 'decode', args.input]
    # Begins a conditional branch to check a condition.
    if args.meta:
        # Executes the statement `argv += ['--meta', args.meta]`.
        argv += ['--meta', args.meta]
    # Begins a conditional branch to check a condition.
    if args.sf is not None:
        # Executes the statement `argv += ['--sf', str(args.sf)]`.
        argv += ['--sf', str(args.sf)]
    # Begins a conditional branch to check a condition.
    if args.bw is not None:
        # Executes the statement `argv += ['--bw', str(args.bw)]`.
        argv += ['--bw', str(args.bw)]
    # Begins a conditional branch to check a condition.
    if args.fs is not None:
        # Executes the statement `argv += ['--fs', str(args.fs)]`.
        argv += ['--fs', str(args.fs)]
    # Begins a conditional branch to check a condition.
    if args.fast:
        # Executes the statement `argv.append('--fast')`.
        argv.append('--fast')
    # Begins a conditional branch to check a condition.
    if args.verbose:
        # Executes the statement `argv.append('--verbose')`.
        argv.append('--verbose')
    # Executes the statement `old = sys.argv`.
    old = sys.argv
    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `sys.argv = argv`.
        sys.argv = argv
        # Returns the computed value to the caller.
        return sdr_lora_cli.main()
    # Defines cleanup code that always runs after try/except.
    finally:
        # Executes the statement `sys.argv = old`.
        sys.argv = old


# Defines the function cmd_batch.
def cmd_batch(args: argparse.Namespace) -> int:
    # Imports specific objects with 'from .sdr_lora_batch_decode import main as batch_main'.
    from .sdr_lora_batch_decode import main as batch_main
    # Executes the statement `argv = ['sdr_lora_batch_decode.py']`.
    argv = ['sdr_lora_batch_decode.py']
    # Begins a conditional branch to check a condition.
    if args.roots:
        # Executes the statement `argv += ['--roots', *args.roots]`.
        argv += ['--roots', *args.roots]
    # Begins a conditional branch to check a condition.
    if args.out:
        # Executes the statement `argv += ['--out', args.out]`.
        argv += ['--out', args.out]
    # Begins a conditional branch to check a condition.
    if args.fast:
        # Executes the statement `argv.append('--fast')`.
        argv.append('--fast')
    # Executes the statement `old = sys.argv`.
    old = sys.argv
    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `sys.argv = argv`.
        sys.argv = argv
        # Returns the computed value to the caller.
        return batch_main()
    # Defines cleanup code that always runs after try/except.
    finally:
        # Executes the statement `sys.argv = old`.
        sys.argv = old


# Defines the function build_parser.
def build_parser() -> argparse.ArgumentParser:
    # Executes the statement `p = argparse.ArgumentParser(description='LoRa Lite PHY - Universal CLI')`.
    p = argparse.ArgumentParser(description='LoRa Lite PHY - Universal CLI')
    # Executes the statement `sub = p.add_subparsers(dest='command', required=True)`.
    sub = p.add_subparsers(dest='command', required=True)

    # decode
    # Executes the statement `pd = sub.add_parser('decode', help='Decode a single IQ file via external/sdr_lora')`.
    pd = sub.add_parser('decode', help='Decode a single IQ file via external/sdr_lora')
    # Executes the statement `pd.add_argument('input', help='Input CF32 IQ file path')`.
    pd.add_argument('input', help='Input CF32 IQ file path')
    # Executes the statement `pd.add_argument('--meta', help='Optional sidecar JSON with LoRa parameters')`.
    pd.add_argument('--meta', help='Optional sidecar JSON with LoRa parameters')
    # Executes the statement `pd.add_argument('--sf', type=int, help='Fallback spreading factor when metadata is missing')`.
    pd.add_argument('--sf', type=int, help='Fallback spreading factor when metadata is missing')
    # Executes the statement `pd.add_argument('--bw', type=int, help='Fallback bandwidth (Hz) when metadata is missing')`.
    pd.add_argument('--bw', type=int, help='Fallback bandwidth (Hz) when metadata is missing')
    # Executes the statement `pd.add_argument('--fs', type=int, help='Fallback sample rate (Hz) when metadata is missing')`.
    pd.add_argument('--fs', type=int, help='Fallback sample rate (Hz) when metadata is missing')
    # Executes the statement `pd.add_argument('--fast', action='store_true', help='Enable fast mode (reduced search)')`.
    pd.add_argument('--fast', action='store_true', help='Enable fast mode (reduced search)')
    # Executes the statement `pd.add_argument('--verbose', '-v', action='store_true')`.
    pd.add_argument('--verbose', '-v', action='store_true')
    # Executes the statement `pd.set_defaults(func=cmd_decode)`.
    pd.set_defaults(func=cmd_decode)

    # batch
    # Executes the statement `pb = sub.add_parser('batch', help='Batch process vectors with the GNU Radio reference')`.
    pb = sub.add_parser('batch', help='Batch process vectors with the GNU Radio reference')
    # Executes the statement `pb.add_argument('--roots', nargs='*', help='Directories to scan for cf32+json pairs')`.
    pb.add_argument('--roots', nargs='*', help='Directories to scan for cf32+json pairs')
    # Executes the statement `pb.add_argument('--out', default='results/sdr_lora_batch.json', help='Where to write the JSON report')`.
    pb.add_argument('--out', default='results/sdr_lora_batch.json', help='Where to write the JSON report')
    # Executes the statement `pb.add_argument('--fast', action='store_true', help='Enable fast mode (limited sweep)')`.
    pb.add_argument('--fast', action='store_true', help='Enable fast mode (limited sweep)')
    # Executes the statement `pb.set_defaults(func=cmd_batch)`.
    pb.set_defaults(func=cmd_batch)

    # Returns the computed value to the caller.
    return p


# Defines the function main.
def main() -> int:
    # Executes the statement `parser = build_parser()`.
    parser = build_parser()
    # Executes the statement `args = parser.parse_args()`.
    args = parser.parse_args()
    # Returns the computed value to the caller.
    return args.func(args)


# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Raises an exception to signal an error.
    raise SystemExit(main())
