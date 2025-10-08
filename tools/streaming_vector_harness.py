#!/usr/bin/env python3
# This file provides the 'streaming vector harness' functionality for the LoRa Lite PHY toolkit.
"""End-to-end streaming harness that generates LoRa vectors, concatenates them with gaps,
then drives the StreamingReceiver chunk-by-chunk to mimic continuous IQ capture."""

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports the module(s) subprocess.
import subprocess
# Imports the module(s) tempfile.
import tempfile
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from typing import Iterable, List, Tuple'.
from typing import Iterable, List, Tuple

# Imports the module(s) numpy as np.
import numpy as np

# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `VECTOR_DIR = ROOT / 'golden_vectors' / 'streaming_harness'`.
VECTOR_DIR = ROOT / 'golden_vectors' / 'streaming_harness'
# Executes the statement `EXPORT_SCRIPT = ROOT / 'external' / 'gr_lora_sdr' / 'scripts' / 'export_tx_reference_vector.py'`.
EXPORT_SCRIPT = ROOT / 'external' / 'gr_lora_sdr' / 'scripts' / 'export_tx_reference_vector.py'

# Executes the statement `DEFAULT_CHUNK = 2048`.
DEFAULT_CHUNK = 2048
# Executes the statement `DEFAULT_GAP_SYMBOLS = 6`.
DEFAULT_GAP_SYMBOLS = 6
# Executes the statement `DEFAULT_VECTOR_COUNT = 5`.
DEFAULT_VECTOR_COUNT = 5

# Executes the statement `SF_OPTIONS = [7, 8, 9]`.
SF_OPTIONS = [7, 8, 9]
# Executes the statement `CR_OPTIONS = [1, 2, 3, 4]`.
CR_OPTIONS = [1, 2, 3, 4]
# Executes the statement `BW_OPTIONS = [125_000, 250_000, 500_000]`.
BW_OPTIONS = [125_000, 250_000, 500_000]


# Defines the function run_export.
def run_export(payload_hex: str,
               # Executes the statement `sf: int,`.
               sf: int,
               # Executes the statement `cr: int,`.
               cr: int,
               # Executes the statement `bandwidth: int,`.
               bandwidth: int,
               # Executes the statement `sample_rate: int,`.
               sample_rate: int,
               # Executes the statement `has_crc: bool,`.
               has_crc: bool,
               # Executes the statement `implicit: bool,`.
               implicit: bool,
               # Executes the statement `out_dir: Path) -> Tuple[Path, dict]:`.
               out_dir: Path) -> Tuple[Path, dict]:
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
        raise RuntimeError(f"Exporter failed: {result.stderr}\n{result.stdout}")

    # Executes the statement `cf32_path: Path | None = None`.
    cf32_path: Path | None = None
    # Starts a loop iterating over a sequence.
    for line in result.stdout.splitlines():
        # Executes the statement `line = line.strip()`.
        line = line.strip()
        # Begins a conditional branch to check a condition.
        if line.startswith('[frame] wrote '):
            # Executes the statement `cf32_path = out_dir / line.split(' ', 2)[2].split()[0]`.
            cf32_path = out_dir / line.split(' ', 2)[2].split()[0]
            # Exits the nearest enclosing loop early.
            break
    # Begins a conditional branch to check a condition.
    if cf32_path is None:
        # Raises an exception to signal an error.
        raise RuntimeError(f"Failed to parse exporter output:\n{result.stdout}")

    # Executes the statement `meta_path = cf32_path.with_suffix('.json')`.
    meta_path = cf32_path.with_suffix('.json')
    # Executes the statement `meta = json.loads(meta_path.read_text())`.
    meta = json.loads(meta_path.read_text())
    # Executes the statement `meta.update({`.
    meta.update({
        # Executes the statement `'sf': sf,`.
        'sf': sf,
        # Executes the statement `'cr': cr,`.
        'cr': cr,
        # Executes the statement `'bw': bandwidth,`.
        'bw': bandwidth,
        # Executes the statement `'sample_rate': sample_rate,`.
        'sample_rate': sample_rate,
        # Executes the statement `'crc': has_crc,`.
        'crc': has_crc,
        # Executes the statement `'implicit_header': implicit,`.
        'implicit_header': implicit,
        # Executes the statement `'payload_hex': payload_hex,`.
        'payload_hex': payload_hex,
    # Executes the statement `})`.
    })
    # Executes the statement `meta_path.write_text(json.dumps(meta, indent=2))`.
    meta_path.write_text(json.dumps(meta, indent=2))
    # Returns the computed value to the caller.
    return cf32_path, meta


# Defines the function generate_vectors.
def generate_vectors(count: int, rng: np.random.Generator, out_dir: Path) -> List[Tuple[Path, dict]]:
    # Executes the statement `out_dir.mkdir(parents=True, exist_ok=True)`.
    out_dir.mkdir(parents=True, exist_ok=True)
    # Executes the statement `vectors: List[Tuple[Path, dict]] = []`.
    vectors: List[Tuple[Path, dict]] = []
    # Starts a loop iterating over a sequence.
    for idx in range(count):
        # Executes the statement `payload = rng.integers(0, 256, rng.integers(8, 24), dtype=np.uint8).tobytes()`.
        payload = rng.integers(0, 256, rng.integers(8, 24), dtype=np.uint8).tobytes()
        # Executes the statement `payload_hex = payload.hex()`.
        payload_hex = payload.hex()
        # Executes the statement `sf = int(rng.choice(SF_OPTIONS))`.
        sf = int(rng.choice(SF_OPTIONS))
        # Executes the statement `cr = int(rng.choice(CR_OPTIONS))`.
        cr = int(rng.choice(CR_OPTIONS))
        # Executes the statement `bw = int(rng.choice(BW_OPTIONS))`.
        bw = int(rng.choice(BW_OPTIONS))
        # Executes the statement `fs = int(bw * rng.choice([2, 4, 8]))`.
        fs = int(bw * rng.choice([2, 4, 8]))
        # Executes the statement `has_crc = bool(rng.integers(0, 2))`.
        has_crc = bool(rng.integers(0, 2))
        # Executes the statement `implicit = bool(rng.integers(0, 2))`.
        implicit = bool(rng.integers(0, 2))
        # Executes the statement `cf32_path, meta = run_export(payload_hex, sf, cr, bw, fs, has_crc, implicit, out_dir)`.
        cf32_path, meta = run_export(payload_hex, sf, cr, bw, fs, has_crc, implicit, out_dir)
        # Executes the statement `vectors.append((cf32_path, meta))`.
        vectors.append((cf32_path, meta))
        # Outputs diagnostic or user-facing text.
        print(f"[gen] {idx+1}/{count}: {cf32_path.name} (sf={sf}, cr={cr}, bw={bw}, fs={fs}, implicit={implicit}, crc={has_crc})")
    # Returns the computed value to the caller.
    return vectors


# Defines the function load_cf32.
def load_cf32(path: Path) -> np.ndarray:
    # Executes the statement `data = np.fromfile(path, dtype=np.complex64)`.
    data = np.fromfile(path, dtype=np.complex64)
    # Returns the computed value to the caller.
    return data


# Defines the function build_capture.
def build_capture(vectors: Iterable[Tuple[np.ndarray, dict]], gap_symbols: int) -> Tuple[np.ndarray, List[int]]:
    # Executes the statement `assembled: List[np.ndarray] = []`.
    assembled: List[np.ndarray] = []
    # Executes the statement `boundaries: List[int] = []`.
    boundaries: List[int] = []
    # Executes the statement `cursor = 0`.
    cursor = 0
    # Starts a loop iterating over a sequence.
    for data, meta in vectors:
        # Executes the statement `assembled.append(data)`.
        assembled.append(data)
        # Executes the statement `cursor += data.size`.
        cursor += data.size
        # Executes the statement `boundaries.append(cursor)`.
        boundaries.append(cursor)
        # Executes the statement `sps = int(meta['sample_rate'] // meta['bw'])`.
        sps = int(meta['sample_rate'] // meta['bw'])
        # Executes the statement `gap_len = gap_symbols * sps`.
        gap_len = gap_symbols * sps
        # Executes the statement `assembled.append(np.zeros(gap_len, dtype=np.complex64))`.
        assembled.append(np.zeros(gap_len, dtype=np.complex64))
        # Executes the statement `cursor += gap_len`.
        cursor += gap_len
    # Returns the computed value to the caller.
    return np.concatenate(assembled), boundaries


# Defines the function run_streaming_decode.
def run_streaming_decode(capture: np.ndarray,
                         # Executes the statement `meta_sequence: List[dict],`.
                         meta_sequence: List[dict],
                         # Executes the statement `boundaries: List[int],`.
                         boundaries: List[int],
                         # Executes the statement `chunk_samples: int,`.
                         chunk_samples: int,
                         # Executes the statement `emit_payload_bytes: bool) -> None:`.
                         emit_payload_bytes: bool) -> None:
    # Imports specific objects with 'from cpp_receiver import streaming_receiver  # hypothetical Python binding'.
    from cpp_receiver import streaming_receiver  # hypothetical Python binding

    # Executes the statement `events_total = 0`.
    events_total = 0
    # Executes the statement `cursor = 0`.
    cursor = 0
    # Executes the statement `srv = streaming_receiver.StreamingReceiver(meta_sequence[0])  # placeholder`.
    srv = streaming_receiver.StreamingReceiver(meta_sequence[0])  # placeholder
    # Executes the statement `segment_idx = 0`.
    segment_idx = 0
    # Starts a loop iterating over a sequence.
    for top in boundaries:
        # Executes the statement `params = meta_sequence[segment_idx]`.
        params = meta_sequence[segment_idx]
        # Executes the statement `chunk = capture[cursor:top]`.
        chunk = capture[cursor:top]
        # Starts a loop that continues while the condition holds.
        while cursor < top:
            # Executes the statement `take = min(chunk_samples, top - cursor)`.
            take = min(chunk_samples, top - cursor)
            # Executes the statement `buf = capture[cursor:cursor + take]`.
            buf = capture[cursor:cursor + take]
            # Executes the statement `events = srv.push_samples(buf)`.
            events = srv.push_samples(buf)
            # Executes the statement `events_total += len(events)`.
            events_total += len(events)
            # Executes the statement `cursor += take`.
            cursor += take
        # Executes the statement `segment_idx += 1`.
        segment_idx += 1
    # Outputs diagnostic or user-facing text.
    print(f"[stream] processed {segment_idx} frames; total events={events_total}")


# Defines the function main.
def main() -> None:
    # Executes the statement `parser = argparse.ArgumentParser(description='Streaming receiver harness for multiple concatenated vectors')`.
    parser = argparse.ArgumentParser(description='Streaming receiver harness for multiple concatenated vectors')
    # Configures the argument parser for the CLI.
    parser.add_argument('--count', type=int, default=DEFAULT_VECTOR_COUNT)
    # Configures the argument parser for the CLI.
    parser.add_argument('--gap-symbols', type=int, default=DEFAULT_GAP_SYMBOLS)
    # Configures the argument parser for the CLI.
    parser.add_argument('--chunk', type=int, default=DEFAULT_CHUNK)
    # Configures the argument parser for the CLI.
    parser.add_argument('--seed', type=int, default=42)
    # Configures the argument parser for the CLI.
    parser.add_argument('--keep', action='store_true', help='Keep generated vectors directory')
    # Executes the statement `args = parser.parse_args()`.
    args = parser.parse_args()

    # Executes the statement `rng = np.random.default_rng(args.seed)`.
    rng = np.random.default_rng(args.seed)

    # Opens a context manager scope for managed resources.
    with tempfile.TemporaryDirectory() as tmpdir:
        # Executes the statement `out_dir = Path(tmpdir)`.
        out_dir = Path(tmpdir)
        # Executes the statement `vectors = generate_vectors(args.count, rng, out_dir)`.
        vectors = generate_vectors(args.count, rng, out_dir)
        # Executes the statement `data_meta = [(load_cf32(path), meta) for path, meta in vectors]`.
        data_meta = [(load_cf32(path), meta) for path, meta in vectors]
        # Executes the statement `capture, boundaries = build_capture(data_meta, args.gap_symbols)`.
        capture, boundaries = build_capture(data_meta, args.gap_symbols)

        # This placeholder assumes we have Python bindings to the streaming receiver.
        # In practice we will replace run_streaming_decode with a call to our C++ harness.
        # Begins a block that monitors for exceptions.
        try:
            # Executes the statement `run_streaming_decode(capture, [meta for _, meta in data_meta], boundaries, args.chunk, emit_payload_bytes=True)`.
            run_streaming_decode(capture, [meta for _, meta in data_meta], boundaries, args.chunk, emit_payload_bytes=True)
        # Handles a specific exception from the try block.
        except ImportError:
            # Outputs diagnostic or user-facing text.
            print('[warn] streaming receiver Python binding not available; capture generated for C++ harness')

        # Begins a conditional branch to check a condition.
        if args.keep:
            # Executes the statement `dest = VECTOR_DIR`.
            dest = VECTOR_DIR
            # Executes the statement `dest.mkdir(parents=True, exist_ok=True)`.
            dest.mkdir(parents=True, exist_ok=True)
            # Starts a loop iterating over a sequence.
            for idx, ((path, meta), boundary) in enumerate(zip(vectors, boundaries), start=1):
                # Executes the statement `payload_hex = meta['payload_hex']`.
                payload_hex = meta['payload_hex']
                # Executes the statement `dest_cf32 = dest / f'vector_{idx:03d}.cf32'`.
                dest_cf32 = dest / f'vector_{idx:03d}.cf32'
                # Executes the statement `dest_json = dest_cf32.with_suffix('.json')`.
                dest_json = dest_cf32.with_suffix('.json')
                # Executes the statement `Path(path).replace(dest_cf32)`.
                Path(path).replace(dest_cf32)
                # Executes the statement `Path(path.with_suffix('.json')).replace(dest_json)`.
                Path(path.with_suffix('.json')).replace(dest_json)
                # Outputs diagnostic or user-facing text.
                print(f"[keep] saved {dest_cf32.name} (payload={payload_hex})")


# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()

