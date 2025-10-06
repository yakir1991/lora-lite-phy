#!/usr/bin/env python3
"""End-to-end streaming harness that generates LoRa vectors, concatenates them with gaps,
then drives the StreamingReceiver chunk-by-chunk to mimic continuous IQ capture."""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path
from typing import Iterable, List, Tuple

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
VECTOR_DIR = ROOT / 'golden_vectors' / 'streaming_harness'
EXPORT_SCRIPT = ROOT / 'external' / 'gr_lora_sdr' / 'scripts' / 'export_tx_reference_vector.py'

DEFAULT_CHUNK = 2048
DEFAULT_GAP_SYMBOLS = 6
DEFAULT_VECTOR_COUNT = 5

SF_OPTIONS = [7, 8, 9]
CR_OPTIONS = [1, 2, 3, 4]
BW_OPTIONS = [125_000, 250_000, 500_000]


def run_export(payload_hex: str,
               sf: int,
               cr: int,
               bandwidth: int,
               sample_rate: int,
               has_crc: bool,
               implicit: bool,
               out_dir: Path) -> Tuple[Path, dict]:
    cmd = [
        'conda', 'run', '-n', 'gr310', 'python', str(EXPORT_SCRIPT),
        '--emit-frame',
        '--sf', str(sf),
        '--cr', str(cr),
        '--bw', str(bandwidth),
        '--samp-rate', str(sample_rate),
        '--payload', '0x' + payload_hex,
        '--out-dir', str(out_dir),
    ]
    if not has_crc:
        cmd.append('--no-crc')
    if implicit:
        cmd.append('--impl-header')
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"Exporter failed: {result.stderr}\n{result.stdout}")

    cf32_path: Path | None = None
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith('[frame] wrote '):
            cf32_path = out_dir / line.split(' ', 2)[2].split()[0]
            break
    if cf32_path is None:
        raise RuntimeError(f"Failed to parse exporter output:\n{result.stdout}")

    meta_path = cf32_path.with_suffix('.json')
    meta = json.loads(meta_path.read_text())
    meta.update({
        'sf': sf,
        'cr': cr,
        'bw': bandwidth,
        'sample_rate': sample_rate,
        'crc': has_crc,
        'implicit_header': implicit,
        'payload_hex': payload_hex,
    })
    meta_path.write_text(json.dumps(meta, indent=2))
    return cf32_path, meta


def generate_vectors(count: int, rng: np.random.Generator, out_dir: Path) -> List[Tuple[Path, dict]]:
    out_dir.mkdir(parents=True, exist_ok=True)
    vectors: List[Tuple[Path, dict]] = []
    for idx in range(count):
        payload = rng.integers(0, 256, rng.integers(8, 24), dtype=np.uint8).tobytes()
        payload_hex = payload.hex()
        sf = int(rng.choice(SF_OPTIONS))
        cr = int(rng.choice(CR_OPTIONS))
        bw = int(rng.choice(BW_OPTIONS))
        fs = int(bw * rng.choice([2, 4, 8]))
        has_crc = bool(rng.integers(0, 2))
        implicit = bool(rng.integers(0, 2))
        cf32_path, meta = run_export(payload_hex, sf, cr, bw, fs, has_crc, implicit, out_dir)
        vectors.append((cf32_path, meta))
        print(f"[gen] {idx+1}/{count}: {cf32_path.name} (sf={sf}, cr={cr}, bw={bw}, fs={fs}, implicit={implicit}, crc={has_crc})")
    return vectors


def load_cf32(path: Path) -> np.ndarray:
    data = np.fromfile(path, dtype=np.complex64)
    return data


def build_capture(vectors: Iterable[Tuple[np.ndarray, dict]], gap_symbols: int) -> Tuple[np.ndarray, List[int]]:
    assembled: List[np.ndarray] = []
    boundaries: List[int] = []
    cursor = 0
    for data, meta in vectors:
        assembled.append(data)
        cursor += data.size
        boundaries.append(cursor)
        sps = int(meta['sample_rate'] // meta['bw'])
        gap_len = gap_symbols * sps
        assembled.append(np.zeros(gap_len, dtype=np.complex64))
        cursor += gap_len
    return np.concatenate(assembled), boundaries


def run_streaming_decode(capture: np.ndarray,
                         meta_sequence: List[dict],
                         boundaries: List[int],
                         chunk_samples: int,
                         emit_payload_bytes: bool) -> None:
    from cpp_receiver import streaming_receiver  # hypothetical Python binding

    events_total = 0
    cursor = 0
    srv = streaming_receiver.StreamingReceiver(meta_sequence[0])  # placeholder
    segment_idx = 0
    for top in boundaries:
        params = meta_sequence[segment_idx]
        chunk = capture[cursor:top]
        while cursor < top:
            take = min(chunk_samples, top - cursor)
            buf = capture[cursor:cursor + take]
            events = srv.push_samples(buf)
            events_total += len(events)
            cursor += take
        segment_idx += 1
    print(f"[stream] processed {segment_idx} frames; total events={events_total}")


def main() -> None:
    parser = argparse.ArgumentParser(description='Streaming receiver harness for multiple concatenated vectors')
    parser.add_argument('--count', type=int, default=DEFAULT_VECTOR_COUNT)
    parser.add_argument('--gap-symbols', type=int, default=DEFAULT_GAP_SYMBOLS)
    parser.add_argument('--chunk', type=int, default=DEFAULT_CHUNK)
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--keep', action='store_true', help='Keep generated vectors directory')
    args = parser.parse_args()

    rng = np.random.default_rng(args.seed)

    with tempfile.TemporaryDirectory() as tmpdir:
        out_dir = Path(tmpdir)
        vectors = generate_vectors(args.count, rng, out_dir)
        data_meta = [(load_cf32(path), meta) for path, meta in vectors]
        capture, boundaries = build_capture(data_meta, args.gap_symbols)

        # This placeholder assumes we have Python bindings to the streaming receiver.
        # In practice we will replace run_streaming_decode with a call to our C++ harness.
        try:
            run_streaming_decode(capture, [meta for _, meta in data_meta], boundaries, args.chunk, emit_payload_bytes=True)
        except ImportError:
            print('[warn] streaming receiver Python binding not available; capture generated for C++ harness')

        if args.keep:
            dest = VECTOR_DIR
            dest.mkdir(parents=True, exist_ok=True)
            for idx, ((path, meta), boundary) in enumerate(zip(vectors, boundaries), start=1):
                payload_hex = meta['payload_hex']
                dest_cf32 = dest / f'vector_{idx:03d}.cf32'
                dest_json = dest_cf32.with_suffix('.json')
                Path(path).replace(dest_cf32)
                Path(path.with_suffix('.json')).replace(dest_json)
                print(f"[keep] saved {dest_cf32.name} (payload={payload_hex})")


if __name__ == '__main__':
    main()

