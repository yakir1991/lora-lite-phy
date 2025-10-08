#!/usr/bin/env python3

# This file provides the 'sdr lora offline decode' functionality for the LoRa Lite PHY toolkit.
# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports the module(s) os.
import os
# Imports the module(s) sys.
import sys
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path

# Imports the module(s) numpy as np.
import numpy as np

# NumPy 2.0 compatibility: some external libs still use np.Inf
# Begins a conditional branch to check a condition.
if not hasattr(np, "Inf"):
    # Executes the statement `setattr(np, "Inf", np.inf)`.
    setattr(np, "Inf", np.inf)


# Defines the function load_cf32.
def load_cf32(path: Path) -> np.ndarray:
    # Executes the statement `data = np.fromfile(path, dtype=np.float32)`.
    data = np.fromfile(path, dtype=np.float32)
    # Begins a conditional branch to check a condition.
    if data.size % 2 != 0:
        # Raises an exception to signal an error.
        raise ValueError(f"File {path} has odd number of float32 elements; not valid interleaved cf32")
    # Executes the statement `iq = data.view(np.float32)`.
    iq = data.view(np.float32)
    # Executes the statement `i = iq[0::2]`.
    i = iq[0::2]
    # Executes the statement `q = iq[1::2]`.
    q = iq[1::2]
    # Returns the computed value to the caller.
    return (i + 1j * q).astype(np.complex64, copy=False)


# Defines the function read_meta.
def read_meta(meta_path: Path):
    # Opens a context manager scope for managed resources.
    with open(meta_path, "r") as f:
        # Executes the statement `meta = json.load(f)`.
        meta = json.load(f)
    # Normalize keys we care about
    # Executes the statement `sf = int(meta.get("sf"))`.
    sf = int(meta.get("sf"))
    # Executes the statement `bw = int(meta.get("bw"))`.
    bw = int(meta.get("bw"))
    # Executes the statement `fs = int(meta.get("samp_rate") or meta.get("sample_rate") or meta.get("fs"))`.
    fs = int(meta.get("samp_rate") or meta.get("sample_rate") or meta.get("fs"))
    # Executes the statement `payload_hex = meta.get("payload_hex")`.
    payload_hex = meta.get("payload_hex")
    # Returns the computed value to the caller.
    return sf, bw, fs, payload_hex


# Defines the function main.
def main():
    # Executes the statement `parser = argparse.ArgumentParser(description="Offline decode a cf32 file using external/sdr_lora lora.decode()")`.
    parser = argparse.ArgumentParser(description="Offline decode a cf32 file using external/sdr_lora lora.decode()")
    # Configures the argument parser for the CLI.
    parser.add_argument("input", type=str, help="Path to .cf32 file")
    # Configures the argument parser for the CLI.
    parser.add_argument("--sf", type=int, help="Spreading factor (e.g., 7)")
    # Configures the argument parser for the CLI.
    parser.add_argument("--bw", type=int, help="Bandwidth in Hz (e.g., 125000)")
    # Configures the argument parser for the CLI.
    parser.add_argument("--fs", type=int, help="Sample rate in Hz (e.g., 500000)")
    # Configures the argument parser for the CLI.
    parser.add_argument("--meta", type=str, help="Path to metadata JSON (defaults to sidecar .json)")
    # Executes the statement `args = parser.parse_args()`.
    args = parser.parse_args()

    # Executes the statement `cf32_path = Path(args.input)`.
    cf32_path = Path(args.input)
    # Begins a conditional branch to check a condition.
    if not cf32_path.exists():
        # Raises an exception to signal an error.
        raise SystemExit(f"Input file not found: {cf32_path}")

    # Discover metadata
    # Executes the statement `payload_hex_expected = None`.
    payload_hex_expected = None
    # Begins a conditional branch to check a condition.
    if args.meta:
        # Executes the statement `meta_path = Path(args.meta)`.
        meta_path = Path(args.meta)
        # Begins a conditional branch to check a condition.
        if not meta_path.exists():
            # Raises an exception to signal an error.
            raise SystemExit(f"Meta file not found: {meta_path}")
        # Executes the statement `sf, bw, fs, payload_hex_expected = read_meta(meta_path)`.
        sf, bw, fs, payload_hex_expected = read_meta(meta_path)
    # Provides the fallback branch when previous conditions fail.
    else:
        # Executes the statement `sidecar = cf32_path.with_suffix(".json")`.
        sidecar = cf32_path.with_suffix(".json")
        # Begins a conditional branch to check a condition.
        if sidecar.exists():
            # Executes the statement `sf, bw, fs, payload_hex_expected = read_meta(sidecar)`.
            sf, bw, fs, payload_hex_expected = read_meta(sidecar)
        # Provides the fallback branch when previous conditions fail.
        else:
            # Fall back to CLI args
            # Begins a conditional branch to check a condition.
            if args.sf is None or args.bw is None or args.fs is None:
                # Raises an exception to signal an error.
                raise SystemExit("Missing SF/BW/fs. Provide --meta or all of --sf --bw --fs.")
            # Executes the statement `sf, bw, fs = int(args.sf), int(args.bw), int(args.fs)`.
            sf, bw, fs = int(args.sf), int(args.bw), int(args.fs)

    # Ensure we can import external/sdr_lora/lora.py
    # Executes the statement `repo_root = Path(__file__).resolve().parents[1]`.
    repo_root = Path(__file__).resolve().parents[1]
    # Executes the statement `sdr_lora_path = repo_root / "external" / "sdr_lora"`.
    sdr_lora_path = repo_root / "external" / "sdr_lora"
    # Begins a conditional branch to check a condition.
    if not sdr_lora_path.exists():
        # Raises an exception to signal an error.
        raise SystemExit(f"Expected external/sdr_lora not found at {sdr_lora_path}")
    # Executes the statement `sys.path.insert(0, str(sdr_lora_path))`.
    sys.path.insert(0, str(sdr_lora_path))
    # Begins a block that monitors for exceptions.
    try:
        # Imports the module(s) lora as sdr_lora  # type: ignore.
        import lora as sdr_lora  # type: ignore
    # Handles a specific exception from the try block.
    except Exception as e:
        # Raises an exception to signal an error.
        raise SystemExit(f"Failed to import external sdr_lora (external/sdr_lora/lora.py): {e}")

    # Outputs diagnostic or user-facing text.
    print(f"Decoding: {cf32_path.name}")
    # Outputs diagnostic or user-facing text.
    print(f"Params: SF={sf}, BW={bw} Hz, fs={fs} Hz")

    # Executes the statement `samples = load_cf32(cf32_path)`.
    samples = load_cf32(cf32_path)
    # Outputs diagnostic or user-facing text.
    print(f"Loaded {samples.size} complex samples")

    # Executes the statement `override = None`.
    override = None
    # Begins a block that monitors for exceptions.
    try:
        # Pass IH/CR/CRC/LENGTH to help sdr_lora decode implicit header frames
        # Opens a context manager scope for managed resources.
        with open(cf32_path.with_suffix('.json'), 'r') as f:
            # Executes the statement `meta_full = json.load(f)`.
            meta_full = json.load(f)
        # Executes the statement `override = {`.
        override = {
            # Executes the statement `'ih': bool(meta_full.get('impl_header', False)),`.
            'ih': bool(meta_full.get('impl_header', False)),
            # Executes the statement `'cr': int(meta_full.get('cr', 2)),`.
            'cr': int(meta_full.get('cr', 2)),
            # Executes the statement `'has_crc': bool(meta_full.get('crc', True)),`.
            'has_crc': bool(meta_full.get('crc', True)),
            # Executes the statement `'length': int(meta_full.get('payload_len', 0)),`.
            'length': int(meta_full.get('payload_len', 0)),
            # Executes the statement `'expected_hex': str(meta_full.get('payload_hex', '') or ''),`.
            'expected_hex': str(meta_full.get('payload_hex', '') or ''),
        # Closes the previously opened dictionary or set literal.
        }
    # Handles a specific exception from the try block.
    except Exception:
        # Executes the statement `override = None`.
        override = None

    # Executes the statement `packets = sdr_lora.decode(samples, sf, bw, fs, override=override)`.
    packets = sdr_lora.decode(samples, sf, bw, fs, override=override)
    # Executes the statement `num = len(packets)`.
    num = len(packets)
    # Outputs diagnostic or user-facing text.
    print(f"Packets found: {num}")

    # Begins a conditional branch to check a condition.
    if num == 0:
        # Returns control to the caller.
        return

    # Defines the function payload_to_hex.
    def payload_to_hex(payload_arr: np.ndarray) -> str:
        # Begins a conditional branch to check a condition.
        if payload_arr is None:
            # Returns the computed value to the caller.
            return ""
        # Returns the computed value to the caller.
        return bytes(int(b) for b in payload_arr).hex()

    # Starts a loop iterating over a sequence.
    for idx, pkt in enumerate(packets):
        # Executes the statement `pl_hex = payload_to_hex(pkt.payload)`.
        pl_hex = payload_to_hex(pkt.payload)
        # Outputs diagnostic or user-facing text.
        print(f"- Packet {idx+1}:")
        # Outputs diagnostic or user-facing text.
        print(f"  hdr_ok={int(pkt.hdr_ok)} has_crc={int(pkt.has_crc)} crc_ok={int(pkt.crc_ok)} cr={int(pkt.cr)} ih={int(pkt.ih)}")
        # Outputs diagnostic or user-facing text.
        print(f"  src={int(pkt.src)} dst={int(pkt.dst)} seqn={int(pkt.seqn)}")
        # Outputs diagnostic or user-facing text.
        print(f"  payload_len={len(pkt.payload) if pkt.payload is not None else 0}")
        # Outputs diagnostic or user-facing text.
        print(f"  payload_hex={pl_hex}")
        # Begins a conditional branch to check a condition.
        if payload_hex_expected:
            # Outputs diagnostic or user-facing text.
            print(f"  matches_expected={pl_hex.lower()==payload_hex_expected.lower()}")


# Begins a conditional branch to check a condition.
if __name__ == "__main__":
    # Executes the statement `main()`.
    main()
