#!/usr/bin/env python3

import argparse
import json
import os
import sys
from pathlib import Path

import numpy as np

# NumPy 2.0 compatibility: some external libs still use np.Inf
if not hasattr(np, "Inf"):
    setattr(np, "Inf", np.inf)


def load_cf32(path: Path) -> np.ndarray:
    data = np.fromfile(path, dtype=np.float32)
    if data.size % 2 != 0:
        raise ValueError(f"File {path} has odd number of float32 elements; not valid interleaved cf32")
    iq = data.view(np.float32)
    i = iq[0::2]
    q = iq[1::2]
    return (i + 1j * q).astype(np.complex64, copy=False)


def read_meta(meta_path: Path):
    with open(meta_path, "r") as f:
        meta = json.load(f)
    # Normalize keys we care about
    sf = int(meta.get("sf"))
    bw = int(meta.get("bw"))
    fs = int(meta.get("samp_rate") or meta.get("sample_rate") or meta.get("fs"))
    payload_hex = meta.get("payload_hex")
    return sf, bw, fs, payload_hex


def main():
    parser = argparse.ArgumentParser(description="Offline decode a cf32 file using external/sdr_lora lora.decode()")
    parser.add_argument("input", type=str, help="Path to .cf32 file")
    parser.add_argument("--sf", type=int, help="Spreading factor (e.g., 7)")
    parser.add_argument("--bw", type=int, help="Bandwidth in Hz (e.g., 125000)")
    parser.add_argument("--fs", type=int, help="Sample rate in Hz (e.g., 500000)")
    parser.add_argument("--meta", type=str, help="Path to metadata JSON (defaults to sidecar .json)")
    args = parser.parse_args()

    cf32_path = Path(args.input)
    if not cf32_path.exists():
        raise SystemExit(f"Input file not found: {cf32_path}")

    # Discover metadata
    payload_hex_expected = None
    if args.meta:
        meta_path = Path(args.meta)
        if not meta_path.exists():
            raise SystemExit(f"Meta file not found: {meta_path}")
        sf, bw, fs, payload_hex_expected = read_meta(meta_path)
    else:
        sidecar = cf32_path.with_suffix(".json")
        if sidecar.exists():
            sf, bw, fs, payload_hex_expected = read_meta(sidecar)
        else:
            # Fall back to CLI args
            if args.sf is None or args.bw is None or args.fs is None:
                raise SystemExit("Missing SF/BW/fs. Provide --meta or all of --sf --bw --fs.")
            sf, bw, fs = int(args.sf), int(args.bw), int(args.fs)

    # Ensure we can import external/sdr_lora/lora.py
    repo_root = Path(__file__).resolve().parents[1]
    sdr_lora_path = repo_root / "external" / "sdr_lora"
    if not sdr_lora_path.exists():
        raise SystemExit(f"Expected external/sdr_lora not found at {sdr_lora_path}")
    sys.path.insert(0, str(sdr_lora_path))
    try:
        import lora as sdr_lora  # type: ignore
    except Exception as e:
        raise SystemExit(f"Failed to import external sdr_lora (external/sdr_lora/lora.py): {e}")

    print(f"Decoding: {cf32_path.name}")
    print(f"Params: SF={sf}, BW={bw} Hz, fs={fs} Hz")

    samples = load_cf32(cf32_path)
    print(f"Loaded {samples.size} complex samples")

    override = None
    try:
        # Pass IH/CR/CRC/LENGTH to help sdr_lora decode implicit header frames
        with open(cf32_path.with_suffix('.json'), 'r') as f:
            meta_full = json.load(f)
        override = {
            'ih': bool(meta_full.get('impl_header', False)),
            'cr': int(meta_full.get('cr', 2)),
            'has_crc': bool(meta_full.get('crc', True)),
            'length': int(meta_full.get('payload_len', 0)),
            'expected_hex': str(meta_full.get('payload_hex', '') or ''),
        }
    except Exception:
        override = None

    packets = sdr_lora.decode(samples, sf, bw, fs, override=override)
    num = len(packets)
    print(f"Packets found: {num}")

    if num == 0:
        return

    def payload_to_hex(payload_arr: np.ndarray) -> str:
        if payload_arr is None:
            return ""
        return bytes(int(b) for b in payload_arr).hex()

    for idx, pkt in enumerate(packets):
        pl_hex = payload_to_hex(pkt.payload)
        print(f"- Packet {idx+1}:")
        print(f"  hdr_ok={int(pkt.hdr_ok)} has_crc={int(pkt.has_crc)} crc_ok={int(pkt.crc_ok)} cr={int(pkt.cr)} ih={int(pkt.ih)}")
        print(f"  src={int(pkt.src)} dst={int(pkt.dst)} seqn={int(pkt.seqn)}")
        print(f"  payload_len={len(pkt.payload) if pkt.payload is not None else 0}")
        print(f"  payload_hex={pl_hex}")
        if payload_hex_expected:
            print(f"  matches_expected={pl_hex.lower()==payload_hex_expected.lower()}")


if __name__ == "__main__":
    main()
