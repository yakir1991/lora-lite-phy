#!/usr/bin/env python3
# This file provides the 'compare header slices with gnur' functionality for the LoRa Lite PHY toolkit.
"""Compare header slices (cf32) against GNU Radio header decode.

For each cf32 in a directory, read its sidecar if available or infer from
filename, run the GNU Radio offline decoder on the slice, and print a parity
report of header fields (length, CR, CRC5 pass/fail) and first payload hex if
present.

Assumptions:
- Slices include preamble through end-of-header (as emitted by streaming receiver)
- Metadata inferred from the original vector (we use a mapping file or a
  simple JSON <slice>.meta if present). If not found, we try to parse parts of
  the original name embedded in the slice filename.
"""
# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports specific objects with 'from dataclasses import dataclass'.
from dataclasses import dataclass
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from typing import Optional, Dict, Any, List'.
from typing import Optional, Dict, Any, List
# Imports the module(s) subprocess.
import subprocess
# Imports the module(s) sys.
import sys

# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `GR_SCRIPT = ROOT / "external/gr_lora_sdr/scripts/decode_offline_recording.py"`.
GR_SCRIPT = ROOT / "external/gr_lora_sdr/scripts/decode_offline_recording.py"
# Executes the statement `VECTORS_DIRS: List[Path] = [`.
VECTORS_DIRS: List[Path] = [
    # Executes the statement `ROOT / 'golden_vectors',`.
    ROOT / 'golden_vectors',
    # Executes the statement `ROOT / 'golden_vectors' / 'new_batch',`.
    ROOT / 'golden_vectors' / 'new_batch',
    # Executes the statement `ROOT / 'golden_vectors' / 'extended_batch',`.
    ROOT / 'golden_vectors' / 'extended_batch',
    # Executes the statement `ROOT / 'golden_vectors' / 'extended_batch_crc_off',`.
    ROOT / 'golden_vectors' / 'extended_batch_crc_off',
    # Executes the statement `ROOT / 'golden_vectors' / 'extended_batch_impl',`.
    ROOT / 'golden_vectors' / 'extended_batch_impl',
# Closes the previously opened list indexing or literal.
]

# Executes the statement `@dataclass`.
@dataclass
# Declares the class SliceMeta.
class SliceMeta:
    # Executes the statement `sf: int`.
    sf: int
    # Executes the statement `bw: int`.
    bw: int
    # Executes the statement `fs: int`.
    fs: int
    # Executes the statement `cr: int`.
    cr: int
    # Executes the statement `has_crc: bool`.
    has_crc: bool
    # Executes the statement `impl_header: bool`.
    impl_header: bool
    # Executes the statement `ldro_mode: int`.
    ldro_mode: int
    # Executes the statement `sync_word: int`.
    sync_word: int
    # Executes the statement `payload_len: int`.
    payload_len: int


# Defines the function run_gnur_on_cf32.
def run_gnur_on_cf32(path: Path, meta: SliceMeta) -> Dict[str, Any]:
    # Executes the statement `cmd = [`.
    cmd = [
        # Executes the statement `sys.executable, str(GR_SCRIPT), str(path),`.
        sys.executable, str(GR_SCRIPT), str(path),
        # Executes the statement `"--sf", str(meta.sf), "--bw", str(meta.bw), "--samp-rate", str(meta.fs),`.
        "--sf", str(meta.sf), "--bw", str(meta.bw), "--samp-rate", str(meta.fs),
        # Executes the statement `"--cr", str(meta.cr), "--ldro-mode", str(meta.ldro_mode), "--format", "cf32",`.
        "--cr", str(meta.cr), "--ldro-mode", str(meta.ldro_mode), "--format", "cf32",
    # Closes the previously opened list indexing or literal.
    ]
    # Executes the statement `cmd.append("--has-crc" if meta.has_crc else "--no-crc")`.
    cmd.append("--has-crc" if meta.has_crc else "--no-crc")
    # Executes the statement `cmd.append("--impl-header" if meta.impl_header else "--explicit-header")`.
    cmd.append("--impl-header" if meta.impl_header else "--explicit-header")
    # Provide a default sync word
    # Executes the statement `cmd.extend(["--sync-word", hex(meta.sync_word)])`.
    cmd.extend(["--sync-word", hex(meta.sync_word)])
    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `res = subprocess.run(cmd, capture_output=True, text=True, timeout=60)`.
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    # Handles a specific exception from the try block.
    except subprocess.TimeoutExpired:
        # Returns the computed value to the caller.
        return {"status": "timeout", "stdout": "", "stderr": ""}
    # Returns the computed value to the caller.
    return {"status": "success" if res.returncode == 0 else "failed", "stdout": res.stdout, "stderr": res.stderr}


# Defines the function parse_meta_from_name.
def parse_meta_from_name(stem: str) -> Optional[SliceMeta]:
    # Best-effort parse of filenames like tx_sf10_bw500000_cr4_crc1_impl0_ldro0_pay10_..._header.cf32
    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `parts = stem.split('_')`.
        parts = stem.split('_')
        # Executes the statement `sf = int([p for p in parts if p.startswith('sf')][0][2:])`.
        sf = int([p for p in parts if p.startswith('sf')][0][2:])
        # Executes the statement `bw = int([p for p in parts if p.startswith('bw')][0][2:])`.
        bw = int([p for p in parts if p.startswith('bw')][0][2:])
        # Executes the statement `cr = int([p for p in parts if p.startswith('cr')][0][2:])`.
        cr = int([p for p in parts if p.startswith('cr')][0][2:])
        # Executes the statement `crc = int([p for p in parts if p.startswith('crc')][0][3:])`.
        crc = int([p for p in parts if p.startswith('crc')][0][3:])
        # Executes the statement `impl = int([p for p in parts if p.startswith('impl')][0][4:])`.
        impl = int([p for p in parts if p.startswith('impl')][0][4:])
        # Executes the statement `ldro = int([p for p in parts if p.startswith('ldro')][0][4:])`.
        ldro = int([p for p in parts if p.startswith('ldro')][0][4:])
        # Returns the computed value to the caller.
        return SliceMeta(sf=sf, bw=bw, fs=bw*8, cr=cr, has_crc=bool(crc), impl_header=bool(impl), ldro_mode=ldro, sync_word=0x12, payload_len=0)
    # Handles a specific exception from the try block.
    except Exception:
        # Returns the computed value to the caller.
        return None


# Defines the function find_original_json.
def find_original_json(base_stem: str) -> Optional[Path]:
    # Search known vectors dirs for a JSON whose stem matches base_stem (without suffixes)
    # Starts a loop iterating over a sequence.
    for root in VECTORS_DIRS:
        # Starts a loop iterating over a sequence.
        for js in root.rglob('*.json'):
            # Begins a conditional branch to check a condition.
            if js.stem == base_stem:
                # Returns the computed value to the caller.
                return js
    # Returns the computed value to the caller.
    return None


# Defines the function load_meta_for_slice.
def load_meta_for_slice(slice_path: Path) -> Optional[SliceMeta]:
    # Prefer <slice>.meta.json if present
    # Executes the statement `meta_file = slice_path.with_suffix(slice_path.suffix + ".meta.json")`.
    meta_file = slice_path.with_suffix(slice_path.suffix + ".meta.json")
    # Begins a conditional branch to check a condition.
    if meta_file.exists():
        # Executes the statement `data = json.loads(meta_file.read_text())`.
        data = json.loads(meta_file.read_text())
        # Normalize fields when loaded from sidecar
        # Returns the computed value to the caller.
        return SliceMeta(
            # Executes the statement `sf=int(data["sf"]),`.
            sf=int(data["sf"]),
            # Executes the statement `bw=int(data["bw"]),`.
            bw=int(data["bw"]),
            # Executes the statement `fs=int(data.get("fs") or data.get("samp_rate") or data.get("sample_rate")),`.
            fs=int(data.get("fs") or data.get("samp_rate") or data.get("sample_rate")),
            # Executes the statement `cr=int(data.get("cr", 1)),`.
            cr=int(data.get("cr", 1)),
            # Executes the statement `has_crc=bool(data.get("has_crc", True)),`.
            has_crc=bool(data.get("has_crc", True)),
            # Executes the statement `impl_header=bool(data.get("impl_header", False)),`.
            impl_header=bool(data.get("impl_header", False)),
            # Executes the statement `ldro_mode=int(data.get("ldro_mode", 0)),`.
            ldro_mode=int(data.get("ldro_mode", 0)),
            # Executes the statement `sync_word=int(data.get("sync_word", 0x12)),`.
            sync_word=int(data.get("sync_word", 0x12)),
            # Executes the statement `payload_len=int(data.get("payload_len", 0)),`.
            payload_len=int(data.get("payload_len", 0)),
        # Closes the previously opened parenthesis grouping.
        )
    # Fallback: try to find original vector JSON by stripping trailing _header
    # Executes the statement `stem = slice_path.stem`.
    stem = slice_path.stem
    # Executes the statement `base = stem[:-7] if stem.endswith('_header') else stem`.
    base = stem[:-7] if stem.endswith('_header') else stem
    # Executes the statement `js = find_original_json(base)`.
    js = find_original_json(base)
    # Begins a conditional branch to check a condition.
    if js and js.exists():
        # Executes the statement `m = json.loads(js.read_text())`.
        m = json.loads(js.read_text())
        # Executes the statement `sf = int(m.get('sf'))`.
        sf = int(m.get('sf'))
        # Executes the statement `bw = int(m.get('bw'))`.
        bw = int(m.get('bw'))
        # Executes the statement `fs = int(m.get('samp_rate') or m.get('sample_rate'))`.
        fs = int(m.get('samp_rate') or m.get('sample_rate'))
        # Executes the statement `cr = int(m.get('cr'))`.
        cr = int(m.get('cr'))
        # Executes the statement `has_crc = bool(m.get('crc', True))`.
        has_crc = bool(m.get('crc', True))
        # Executes the statement `impl = bool(m.get('impl_header') or m.get('implicit_header'))`.
        impl = bool(m.get('impl_header') or m.get('implicit_header'))
        # Executes the statement `ldro = int(m.get('ldro_mode', 0))`.
        ldro = int(m.get('ldro_mode', 0))
        # Executes the statement `sync_word = int(m.get('sync_word', 0x12))`.
        sync_word = int(m.get('sync_word', 0x12))
        # Executes the statement `pay_len = int(m.get('payload_len') or m.get('payload_length') or 0)`.
        pay_len = int(m.get('payload_len') or m.get('payload_length') or 0)
        # Returns the computed value to the caller.
        return SliceMeta(sf=sf, bw=bw, fs=fs, cr=cr, has_crc=has_crc, impl_header=impl, ldro_mode=ldro, sync_word=sync_word, payload_len=pay_len)
    # Last resort: parse from name with heuristic fs
    # Returns the computed value to the caller.
    return parse_meta_from_name(stem)


# Defines the function main.
def main() -> None:
    # Executes the statement `ap = argparse.ArgumentParser(description="Compare header slices against GNU Radio decode")`.
    ap = argparse.ArgumentParser(description="Compare header slices against GNU Radio decode")
    # Executes the statement `ap.add_argument('--dir', type=Path, default=ROOT / 'results' / 'hdr_slices')`.
    ap.add_argument('--dir', type=Path, default=ROOT / 'results' / 'hdr_slices')
    # Executes the statement `ap.add_argument('--limit', type=int, default=20)`.
    ap.add_argument('--limit', type=int, default=20)
    # Executes the statement `args = ap.parse_args()`.
    args = ap.parse_args()

    # Begins a conditional branch to check a condition.
    if not GR_SCRIPT.exists():
        # Raises an exception to signal an error.
        raise SystemExit("GNU Radio script not found: " + str(GR_SCRIPT))

    # Executes the statement `slices = sorted(args.dir.glob('*.cf32'))`.
    slices = sorted(args.dir.glob('*.cf32'))
    # Executes the statement `slices = slices[: args.limit]`.
    slices = slices[: args.limit]
    # Begins a conditional branch to check a condition.
    if not slices:
        # Outputs diagnostic or user-facing text.
        print('No header slices found in', args.dir)
        # Returns control to the caller.
        return

    # Starts a loop iterating over a sequence.
    for s in slices:
        # Executes the statement `meta = load_meta_for_slice(s)`.
        meta = load_meta_for_slice(s)
        # Begins a conditional branch to check a condition.
        if not meta:
            # Outputs diagnostic or user-facing text.
            print("Skip (no meta)", s.name)
            # Skips to the next iteration of the loop.
            continue
        # Executes the statement `res = run_gnur_on_cf32(s, meta)`.
        res = run_gnur_on_cf32(s, meta)
        # Executes the statement `status = res.get('status')`.
        status = res.get('status')
        # Outputs diagnostic or user-facing text.
        print(f"{s.name}: GNUR status={status}")
        # Begins a conditional branch to check a condition.
        if status == 'success':
            # Print first two lines from stdout for quick parity
            # Executes the statement `lines = [ln for ln in res['stdout'].splitlines() if ln.strip()][:2]`.
            lines = [ln for ln in res['stdout'].splitlines() if ln.strip()][:2]
            # Starts a loop iterating over a sequence.
            for ln in lines:
                # Outputs diagnostic or user-facing text.
                print('  ', ln)

# Begins a conditional branch to check a condition.
if __name__ == '__main__':
    # Executes the statement `main()`.
    main()
