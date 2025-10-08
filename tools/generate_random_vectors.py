#!/usr/bin/env python3
# This file provides the 'generate random vectors' functionality for the LoRa Lite PHY toolkit.
"""Generate random LoRa vectors via GNU Radio exporter."""

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

# Imports the module(s) numpy as np.
import numpy as np

# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `EXPORT_SCRIPT = ROOT / 'external' / 'gr_lora_sdr' / 'scripts' / 'export_tx_reference_vector.py'`.
EXPORT_SCRIPT = ROOT / 'external' / 'gr_lora_sdr' / 'scripts' / 'export_tx_reference_vector.py'
# Executes the statement `DEFAULT_OUT = ROOT / 'golden_vectors' / 'random_set'`.
DEFAULT_OUT = ROOT / 'golden_vectors' / 'random_set'

# Executes the statement `DEFAULT_BW = 125_000`.
DEFAULT_BW = 125_000
# Executes the statement `DEFAULT_SAMP_RATE = 500_000`.
DEFAULT_SAMP_RATE = 500_000

# Executes the statement `SF_OPTIONS = [7, 8, 9, 10, 11, 12]`.
SF_OPTIONS = [7, 8, 9, 10, 11, 12]
# Executes the statement `CR_OPTIONS = [1, 2, 3, 4]`.
CR_OPTIONS = [1, 2, 3, 4]
# Executes the statement `LDRO_OPTIONS = [0, 1, 2]`.
LDRO_OPTIONS = [0, 1, 2]


# Defines the function random_payload.
def random_payload(rng: np.random.Generator, length: int) -> bytes:
    # Executes the statement `data = rng.integers(0, 256, length, dtype=np.uint8)`.
    data = rng.integers(0, 256, length, dtype=np.uint8)
    # Returns the computed value to the caller.
    return data.tobytes()


# Defines the function run_export.
def run_export(payload_hex: str,
               # Executes the statement `sf: int,`.
               sf: int,
               # Executes the statement `cr: int,`.
               cr: int,
               # Executes the statement `has_crc: bool,`.
               has_crc: bool,
               # Executes the statement `implicit: bool,`.
               implicit: bool,
               # Executes the statement `ldro: int,`.
               ldro: int,
               # Executes the statement `out_dir: Path,`.
               out_dir: Path,
               # Executes the statement `*,`.
               *,
               # Executes the statement `bandwidth: int,`.
               bandwidth: int,
               # Executes the statement `sample_rate: int) -> Path:`.
               sample_rate: int) -> Path:
    # Executes the statement `cmd = [`.
    cmd = [
        # Executes the statement `'conda', 'run', '-n', 'gr310', 'python', str(EXPORT_SCRIPT),`.
        'conda', 'run', '-n', 'gr310', 'python', str(EXPORT_SCRIPT),
        # Executes the statement `'--emit-frame',`.
        '--emit-frame',
        # Executes the statement `'--sf', str(sf),`.
        '--sf', str(sf),
        # Executes the statement `'--cr', str(cr),`.
        '--cr', str(cr),
        # Executes the statement `'--bw', str(bandwidth),`.
        '--bw', str(bandwidth),
        # Executes the statement `'--samp-rate', str(sample_rate),`.
        '--samp-rate', str(sample_rate),
        # Executes the statement `'--payload', '0x' + payload_hex,`.
        '--payload', '0x' + payload_hex,
        # Executes the statement `'--out-dir', str(out_dir),`.
        '--out-dir', str(out_dir),
        # Executes the statement `'--ldro-mode', str(ldro),`.
        '--ldro-mode', str(ldro),
    # Closes the previously opened list indexing or literal.
    ]
    # Begins a conditional branch to check a condition.
    if not has_crc:
        # Executes the statement `cmd.append('--no-crc')`.
        cmd.append('--no-crc')
    # Begins a conditional branch to check a condition.
    if implicit:
        # Executes the statement `cmd.append('--impl-header')`.
        cmd.append('--impl-header')
    # Executes the statement `result = subprocess.run(cmd, capture_output=True, text=True)`.
    result = subprocess.run(cmd, capture_output=True, text=True)
    # Begins a conditional branch to check a condition.
    if result.returncode != 0:
        # Raises an exception to signal an error.
        raise RuntimeError(f"export script failed: {result.stderr}\n{result.stdout}")
    # Extract generated filename from stdout
    # Starts a loop iterating over a sequence.
    for line in result.stdout.splitlines():
        # Executes the statement `line = line.strip()`.
        line = line.strip()
        # Begins a conditional branch to check a condition.
        if line.startswith('[frame] wrote '):
            # Executes the statement `fname = line.split(' ', 2)[2].split()[0]`.
            fname = line.split(' ', 2)[2].split()[0]
            # Returns the computed value to the caller.
            return out_dir / fname
    # Raises an exception to signal an error.
    raise RuntimeError(f"Could not parse output file from exporter:\n{result.stdout}")


# Defines the function enrich_metadata.
def enrich_metadata(path: Path, extra: dict) -> None:
    # Executes the statement `meta_path = path.with_suffix('.json')`.
    meta_path = path.with_suffix('.json')
    # Executes the statement `data = json.loads(meta_path.read_text())`.
    data = json.loads(meta_path.read_text())
    # Executes the statement `data.update(extra)`.
    data.update(extra)
    # Executes the statement `meta_path.write_text(json.dumps(data, indent=2))`.
    meta_path.write_text(json.dumps(data, indent=2))


# Defines the function main.
def main() -> None:
    # Executes the statement `parser = argparse.ArgumentParser(description='Generate random LoRa vectors using GNU Radio exporter')`.
    parser = argparse.ArgumentParser(description='Generate random LoRa vectors using GNU Radio exporter')
    # Configures the argument parser for the CLI.
    parser.add_argument('--count', type=int, default=30, help='Number of vectors to generate')
    # Configures the argument parser for the CLI.
    parser.add_argument('--output', type=Path, default=DEFAULT_OUT, help='Output directory')
    # Configures the argument parser for the CLI.
    parser.add_argument('--seed', type=int, default=12345, help='RNG seed')
    # Configures the argument parser for the CLI.
    parser.add_argument('--min-len', type=int, default=5, help='Minimum payload length (bytes)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--max-len', type=int, default=20, help='Maximum payload length (bytes)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--sf', type=int, help='Force a specific spreading factor')
    # Configures the argument parser for the CLI.
    parser.add_argument('--cr', type=int, help='Force a specific coding rate index (1..4)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--ldro-mode', type=int, choices=[0, 1, 2], help='Force LDRO mode (0,1,2)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--bw', type=int, default=DEFAULT_BW, help='Channel bandwidth in Hz')
    # Configures the argument parser for the CLI.
    parser.add_argument('--sample-rate', type=int, default=DEFAULT_SAMP_RATE, help='Sample rate in Hz (controls oversampling factor)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--implicit-header', action='store_true', help='Generate implicit-header frame')
    # Configures the argument parser for the CLI.
    parser.add_argument('--no-crc', action='store_true', help='Disable CRC in generated frame')
    # Configures the argument parser for the CLI.
    parser.add_argument('--message', help='ASCII message to encode as payload (overrides random payload)')
    # Configures the argument parser for the CLI.
    parser.add_argument('--payload-hex', help='Hexadecimal payload bytes (overrides --message and random payload)')
    # Executes the statement `args = parser.parse_args()`.
    args = parser.parse_args()

    # Executes the statement `out_dir = args.output.resolve()`.
    out_dir = args.output.resolve()
    # Executes the statement `out_dir.mkdir(parents=True, exist_ok=True)`.
    out_dir.mkdir(parents=True, exist_ok=True)

    # Executes the statement `manual_payload = args.payload_hex`.
    manual_payload = args.payload_hex
    # Begins a conditional branch to check a condition.
    if args.message and not manual_payload:
        # Executes the statement `manual_payload = args.message.encode('utf-8').hex()`.
        manual_payload = args.message.encode('utf-8').hex()

    # Executes the statement `manual_mode = any([`.
    manual_mode = any([
        # Executes the statement `manual_payload,`.
        manual_payload,
        # Accesses a parsed command-line argument.
        args.sf is not None,
        # Accesses a parsed command-line argument.
        args.cr is not None,
        # Accesses a parsed command-line argument.
        args.ldro_mode is not None,
        # Accesses a parsed command-line argument.
        args.implicit_header,
        # Accesses a parsed command-line argument.
        args.no_crc,
        # Accesses a parsed command-line argument.
        args.sample_rate != DEFAULT_SAMP_RATE,
        # Accesses a parsed command-line argument.
        args.bw != DEFAULT_BW,
    # Executes the statement `])`.
    ])

    # Begins a conditional branch to check a condition.
    if manual_mode:
        # Executes the statement `out_dir.mkdir(parents=True, exist_ok=True)`.
        out_dir.mkdir(parents=True, exist_ok=True)
        # Executes the statement `payload_hex = manual_payload or random_payload(np.random.default_rng(args.seed), args.min_len).hex()`.
        payload_hex = manual_payload or random_payload(np.random.default_rng(args.seed), args.min_len).hex()
        # Executes the statement `sf = args.sf or SF_OPTIONS[0]`.
        sf = args.sf or SF_OPTIONS[0]
        # Executes the statement `cr = args.cr or CR_OPTIONS[0]`.
        cr = args.cr or CR_OPTIONS[0]
        # Executes the statement `ldro = args.ldro_mode if args.ldro_mode is not None else 0`.
        ldro = args.ldro_mode if args.ldro_mode is not None else 0
        # Begins a conditional branch to check a condition.
        if ldro == 2 and sf < 11:
            # Executes the statement `ldro = 0`.
            ldro = 0
        # Executes the statement `has_crc = not args.no_crc`.
        has_crc = not args.no_crc
        # Executes the statement `implicit = args.implicit_header`.
        implicit = args.implicit_header
        # Executes the statement `cf32_path = run_export(payload_hex, sf, cr, has_crc, implicit, ldro, out_dir,`.
        cf32_path = run_export(payload_hex, sf, cr, has_crc, implicit, ldro, out_dir,
                               # Executes the statement `bandwidth=args.bw, sample_rate=args.sample_rate)`.
                               bandwidth=args.bw, sample_rate=args.sample_rate)
        # Executes the statement `enrich_metadata(cf32_path, {`.
        enrich_metadata(cf32_path, {
            # Executes the statement `'generator': 'custom_gnuradio',`.
            'generator': 'custom_gnuradio',
            # Executes the statement `'message': args.message if args.message else None,`.
            'message': args.message if args.message else None,
            # Executes the statement `'payload_hex': payload_hex,`.
            'payload_hex': payload_hex,
        # Executes the statement `})`.
        })
        # Outputs diagnostic or user-facing text.
        print(f"[info] generated {cf32_path.name} (sf={sf}, cr={cr}, ldro={ldro}, implicit={implicit}, crc={has_crc}, bw={args.bw}, fs={args.sample_rate})")
        # Returns control to the caller.
        return

    # Random generation path (legacy behaviour)
    # Clear previous random files
    # Starts a loop iterating over a sequence.
    for f in out_dir.glob('tx_*'):  # the exporter produces tx_* files
        # Executes the statement `f.unlink()`.
        f.unlink()

    # Executes the statement `rng = np.random.default_rng(args.seed)`.
    rng = np.random.default_rng(args.seed)

    # Executes the statement `generated = 0`.
    generated = 0
    # Executes the statement `attempts = 0`.
    attempts = 0
    # Starts a loop that continues while the condition holds.
    while generated < args.count:
        # Executes the statement `attempts += 1`.
        attempts += 1
        # Executes the statement `sf = args.sf if args.sf is not None else int(rng.choice(SF_OPTIONS))`.
        sf = args.sf if args.sf is not None else int(rng.choice(SF_OPTIONS))
        # Executes the statement `cr = args.cr if args.cr is not None else int(rng.choice(CR_OPTIONS))`.
        cr = args.cr if args.cr is not None else int(rng.choice(CR_OPTIONS))
        # Executes the statement `ldro = args.ldro_mode if args.ldro_mode is not None else int(rng.choice(LDRO_OPTIONS))`.
        ldro = args.ldro_mode if args.ldro_mode is not None else int(rng.choice(LDRO_OPTIONS))
        # Begins a conditional branch to check a condition.
        if ldro == 2 and sf < 11:
            # Executes the statement `ldro = 0`.
            ldro = 0
        # Executes the statement `has_crc = False if args.no_crc else bool(rng.integers(0, 2))`.
        has_crc = False if args.no_crc else bool(rng.integers(0, 2))
        # Executes the statement `implicit = True if args.implicit_header else bool(rng.integers(0, 2))`.
        implicit = True if args.implicit_header else bool(rng.integers(0, 2))
        # Executes the statement `pay_len = int(rng.integers(args.min_len, args.max_len + 1))`.
        pay_len = int(rng.integers(args.min_len, args.max_len + 1))
        # Executes the statement `payload = random_payload(rng, pay_len)`.
        payload = random_payload(rng, pay_len)
        # Executes the statement `payload_hex = payload.hex()`.
        payload_hex = payload.hex()
        # Begins a block that monitors for exceptions.
        try:
            # Executes the statement `cf32_path = run_export(payload_hex, sf, cr, has_crc, implicit, ldro, out_dir,`.
            cf32_path = run_export(payload_hex, sf, cr, has_crc, implicit, ldro, out_dir,
                                   # Executes the statement `bandwidth=args.bw, sample_rate=args.sample_rate)`.
                                   bandwidth=args.bw, sample_rate=args.sample_rate)
        # Handles a specific exception from the try block.
        except RuntimeError as exc:
            # Outputs diagnostic or user-facing text.
            print(f"[warn] export failed ({exc}); retrying...")
            # Skips to the next iteration of the loop.
            continue
        # Executes the statement `enrich_metadata(cf32_path, {`.
        enrich_metadata(cf32_path, {
            # Executes the statement `'generator': 'random_gnuradio',`.
            'generator': 'random_gnuradio',
            # Executes the statement `'random_seed': args.seed,`.
            'random_seed': args.seed,
        # Executes the statement `})`.
        })
        # Executes the statement `generated += 1`.
        generated += 1
        # Outputs diagnostic or user-facing text.
        print(f"[info] generated {cf32_path.name} (sf={sf}, cr={cr}, ldro={ldro}, implicit={implicit}, crc={has_crc}, len={pay_len})")

    # Outputs diagnostic or user-facing text.
    print(f"Generated {generated} vectors under {out_dir} (attempts={attempts})")


# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
