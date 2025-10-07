#!/usr/bin/env python3
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
from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Dict, Any, List
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
GR_SCRIPT = ROOT / "external/gr_lora_sdr/scripts/decode_offline_recording.py"
VECTORS_DIRS: List[Path] = [
    ROOT / 'golden_vectors',
    ROOT / 'golden_vectors' / 'new_batch',
    ROOT / 'golden_vectors' / 'extended_batch',
    ROOT / 'golden_vectors' / 'extended_batch_crc_off',
    ROOT / 'golden_vectors' / 'extended_batch_impl',
]

@dataclass
class SliceMeta:
    sf: int
    bw: int
    fs: int
    cr: int
    has_crc: bool
    impl_header: bool
    ldro_mode: int
    sync_word: int
    payload_len: int


def run_gnur_on_cf32(path: Path, meta: SliceMeta) -> Dict[str, Any]:
    cmd = [
        sys.executable, str(GR_SCRIPT), str(path),
        "--sf", str(meta.sf), "--bw", str(meta.bw), "--samp-rate", str(meta.fs),
        "--cr", str(meta.cr), "--ldro-mode", str(meta.ldro_mode), "--format", "cf32",
    ]
    cmd.append("--has-crc" if meta.has_crc else "--no-crc")
    cmd.append("--impl-header" if meta.impl_header else "--explicit-header")
    # Provide a default sync word
    cmd.extend(["--sync-word", hex(meta.sync_word)])
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return {"status": "timeout", "stdout": "", "stderr": ""}
    return {"status": "success" if res.returncode == 0 else "failed", "stdout": res.stdout, "stderr": res.stderr}


def parse_meta_from_name(stem: str) -> Optional[SliceMeta]:
    # Best-effort parse of filenames like tx_sf10_bw500000_cr4_crc1_impl0_ldro0_pay10_..._header.cf32
    try:
        parts = stem.split('_')
        sf = int([p for p in parts if p.startswith('sf')][0][2:])
        bw = int([p for p in parts if p.startswith('bw')][0][2:])
        cr = int([p for p in parts if p.startswith('cr')][0][2:])
        crc = int([p for p in parts if p.startswith('crc')][0][3:])
        impl = int([p for p in parts if p.startswith('impl')][0][4:])
        ldro = int([p for p in parts if p.startswith('ldro')][0][4:])
        return SliceMeta(sf=sf, bw=bw, fs=bw*8, cr=cr, has_crc=bool(crc), impl_header=bool(impl), ldro_mode=ldro, sync_word=0x12, payload_len=0)
    except Exception:
        return None


def find_original_json(base_stem: str) -> Optional[Path]:
    # Search known vectors dirs for a JSON whose stem matches base_stem (without suffixes)
    for root in VECTORS_DIRS:
        for js in root.rglob('*.json'):
            if js.stem == base_stem:
                return js
    return None


def load_meta_for_slice(slice_path: Path) -> Optional[SliceMeta]:
    # Prefer <slice>.meta.json if present
    meta_file = slice_path.with_suffix(slice_path.suffix + ".meta.json")
    if meta_file.exists():
        data = json.loads(meta_file.read_text())
        # Normalize fields when loaded from sidecar
        return SliceMeta(
            sf=int(data["sf"]),
            bw=int(data["bw"]),
            fs=int(data.get("fs") or data.get("samp_rate") or data.get("sample_rate")),
            cr=int(data.get("cr", 1)),
            has_crc=bool(data.get("has_crc", True)),
            impl_header=bool(data.get("impl_header", False)),
            ldro_mode=int(data.get("ldro_mode", 0)),
            sync_word=int(data.get("sync_word", 0x12)),
            payload_len=int(data.get("payload_len", 0)),
        )
    # Fallback: try to find original vector JSON by stripping trailing _header
    stem = slice_path.stem
    base = stem[:-7] if stem.endswith('_header') else stem
    js = find_original_json(base)
    if js and js.exists():
        m = json.loads(js.read_text())
        sf = int(m.get('sf'))
        bw = int(m.get('bw'))
        fs = int(m.get('samp_rate') or m.get('sample_rate'))
        cr = int(m.get('cr'))
        has_crc = bool(m.get('crc', True))
        impl = bool(m.get('impl_header') or m.get('implicit_header'))
        ldro = int(m.get('ldro_mode', 0))
        sync_word = int(m.get('sync_word', 0x12))
        pay_len = int(m.get('payload_len') or m.get('payload_length') or 0)
        return SliceMeta(sf=sf, bw=bw, fs=fs, cr=cr, has_crc=has_crc, impl_header=impl, ldro_mode=ldro, sync_word=sync_word, payload_len=pay_len)
    # Last resort: parse from name with heuristic fs
    return parse_meta_from_name(stem)


def main() -> None:
    ap = argparse.ArgumentParser(description="Compare header slices against GNU Radio decode")
    ap.add_argument('--dir', type=Path, default=ROOT / 'results' / 'hdr_slices')
    ap.add_argument('--limit', type=int, default=20)
    args = ap.parse_args()

    if not GR_SCRIPT.exists():
        raise SystemExit("GNU Radio script not found: " + str(GR_SCRIPT))

    slices = sorted(args.dir.glob('*.cf32'))
    slices = slices[: args.limit]
    if not slices:
        print('No header slices found in', args.dir)
        return

    for s in slices:
        meta = load_meta_for_slice(s)
        if not meta:
            print("Skip (no meta)", s.name)
            continue
        res = run_gnur_on_cf32(s, meta)
        status = res.get('status')
        print(f"{s.name}: GNUR status={status}")
        if status == 'success':
            # Print first two lines from stdout for quick parity
            lines = [ln for ln in res['stdout'].splitlines() if ln.strip()][:2]
            for ln in lines:
                print('  ', ln)

if __name__ == '__main__':
    main()
