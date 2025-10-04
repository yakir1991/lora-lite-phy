#!/usr/bin/env python3
"""Generate random LoRa vectors via GNU Radio exporter."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
EXPORT_SCRIPT = ROOT / 'external' / 'gr_lora_sdr' / 'scripts' / 'export_tx_reference_vector.py'
DEFAULT_OUT = ROOT / 'golden_vectors' / 'random_set'

DEFAULT_BW = 125_000
DEFAULT_SAMP_RATE = 500_000

SF_OPTIONS = [7, 8, 9, 10, 11, 12]
CR_OPTIONS = [1, 2, 3, 4]
LDRO_OPTIONS = [0, 1, 2]


def random_payload(rng: np.random.Generator, length: int) -> bytes:
    data = rng.integers(0, 256, length, dtype=np.uint8)
    return data.tobytes()


def run_export(payload_hex: str,
               sf: int,
               cr: int,
               has_crc: bool,
               implicit: bool,
               ldro: int,
               out_dir: Path,
               *,
               bandwidth: int,
               sample_rate: int) -> Path:
    cmd = [
        'conda', 'run', '-n', 'gr310', 'python', str(EXPORT_SCRIPT),
        '--emit-frame',
        '--sf', str(sf),
        '--cr', str(cr),
        '--bw', str(bandwidth),
        '--samp-rate', str(sample_rate),
        '--payload', '0x' + payload_hex,
        '--out-dir', str(out_dir),
        '--ldro-mode', str(ldro),
    ]
    if not has_crc:
        cmd.append('--no-crc')
    if implicit:
        cmd.append('--impl-header')
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"export script failed: {result.stderr}\n{result.stdout}")
    # Extract generated filename from stdout
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith('[frame] wrote '):
            fname = line.split(' ', 2)[2].split()[0]
            return out_dir / fname
    raise RuntimeError(f"Could not parse output file from exporter:\n{result.stdout}")


def enrich_metadata(path: Path, extra: dict) -> None:
    meta_path = path.with_suffix('.json')
    data = json.loads(meta_path.read_text())
    data.update(extra)
    meta_path.write_text(json.dumps(data, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser(description='Generate random LoRa vectors using GNU Radio exporter')
    parser.add_argument('--count', type=int, default=30, help='Number of vectors to generate')
    parser.add_argument('--output', type=Path, default=DEFAULT_OUT, help='Output directory')
    parser.add_argument('--seed', type=int, default=12345, help='RNG seed')
    parser.add_argument('--min-len', type=int, default=5, help='Minimum payload length (bytes)')
    parser.add_argument('--max-len', type=int, default=20, help='Maximum payload length (bytes)')
    parser.add_argument('--sf', type=int, help='Force a specific spreading factor')
    parser.add_argument('--cr', type=int, help='Force a specific coding rate index (1..4)')
    parser.add_argument('--ldro-mode', type=int, choices=[0, 1, 2], help='Force LDRO mode (0,1,2)')
    parser.add_argument('--bw', type=int, default=DEFAULT_BW, help='Channel bandwidth in Hz')
    parser.add_argument('--sample-rate', type=int, default=DEFAULT_SAMP_RATE, help='Sample rate in Hz (controls oversampling factor)')
    parser.add_argument('--implicit-header', action='store_true', help='Generate implicit-header frame')
    parser.add_argument('--no-crc', action='store_true', help='Disable CRC in generated frame')
    parser.add_argument('--message', help='ASCII message to encode as payload (overrides random payload)')
    parser.add_argument('--payload-hex', help='Hexadecimal payload bytes (overrides --message and random payload)')
    args = parser.parse_args()

    out_dir = args.output.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    manual_payload = args.payload_hex
    if args.message and not manual_payload:
        manual_payload = args.message.encode('utf-8').hex()

    manual_mode = any([
        manual_payload,
        args.sf is not None,
        args.cr is not None,
        args.ldro_mode is not None,
        args.implicit_header,
        args.no_crc,
        args.sample_rate != DEFAULT_SAMP_RATE,
        args.bw != DEFAULT_BW,
    ])

    if manual_mode:
        out_dir.mkdir(parents=True, exist_ok=True)
        payload_hex = manual_payload or random_payload(np.random.default_rng(args.seed), args.min_len).hex()
        sf = args.sf or SF_OPTIONS[0]
        cr = args.cr or CR_OPTIONS[0]
        ldro = args.ldro_mode if args.ldro_mode is not None else 0
        if ldro == 2 and sf < 11:
            ldro = 0
        has_crc = not args.no_crc
        implicit = args.implicit_header
        cf32_path = run_export(payload_hex, sf, cr, has_crc, implicit, ldro, out_dir,
                               bandwidth=args.bw, sample_rate=args.sample_rate)
        enrich_metadata(cf32_path, {
            'generator': 'custom_gnuradio',
            'message': args.message if args.message else None,
            'payload_hex': payload_hex,
        })
        print(f"[info] generated {cf32_path.name} (sf={sf}, cr={cr}, ldro={ldro}, implicit={implicit}, crc={has_crc}, bw={args.bw}, fs={args.sample_rate})")
        return

    # Random generation path (legacy behaviour)
    # Clear previous random files
    for f in out_dir.glob('tx_*'):  # the exporter produces tx_* files
        f.unlink()

    rng = np.random.default_rng(args.seed)

    generated = 0
    attempts = 0
    while generated < args.count:
        attempts += 1
        sf = args.sf if args.sf is not None else int(rng.choice(SF_OPTIONS))
        cr = args.cr if args.cr is not None else int(rng.choice(CR_OPTIONS))
        ldro = args.ldro_mode if args.ldro_mode is not None else int(rng.choice(LDRO_OPTIONS))
        if ldro == 2 and sf < 11:
            ldro = 0
        has_crc = False if args.no_crc else bool(rng.integers(0, 2))
        implicit = True if args.implicit_header else bool(rng.integers(0, 2))
        pay_len = int(rng.integers(args.min_len, args.max_len + 1))
        payload = random_payload(rng, pay_len)
        payload_hex = payload.hex()
        try:
            cf32_path = run_export(payload_hex, sf, cr, has_crc, implicit, ldro, out_dir,
                                   bandwidth=args.bw, sample_rate=args.sample_rate)
        except RuntimeError as exc:
            print(f"[warn] export failed ({exc}); retrying...")
            continue
        enrich_metadata(cf32_path, {
            'generator': 'random_gnuradio',
            'random_seed': args.seed,
        })
        generated += 1
        print(f"[info] generated {cf32_path.name} (sf={sf}, cr={cr}, ldro={ldro}, implicit={implicit}, crc={has_crc}, len={pay_len})")

    print(f"Generated {generated} vectors under {out_dir} (attempts={attempts})")


if __name__ == '__main__':
    main()
