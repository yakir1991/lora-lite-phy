#!/usr/bin/env python3
# This file provides the 'sdr lora batch decode' functionality for the LoRa Lite PHY toolkit.
# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports the module(s) sys.
import sys
# Imports the module(s) time.
import time
# Imports specific objects with 'from typing import List, Tuple, Optional, Dict, Any'.
from typing import List, Tuple, Optional, Dict, Any

# Imports the module(s) numpy as np.
import numpy as np

# Allow importing external/sdr_lora
# Executes the statement `ROOT = Path(__file__).resolve().parents[1]`.
ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `SDR_LORA = ROOT / "external" / "sdr_lora"`.
SDR_LORA = ROOT / "external" / "sdr_lora"
# Begins a conditional branch to check a condition.
if str(SDR_LORA) not in sys.path:
    # Executes the statement `sys.path.insert(0, str(SDR_LORA))`.
    sys.path.insert(0, str(SDR_LORA))
# Add project root for importing our receiver
# Begins a conditional branch to check a condition.
if str(ROOT) not in sys.path:
    # Executes the statement `sys.path.insert(0, str(ROOT))`.
    sys.path.insert(0, str(ROOT))

# NumPy 2.0 compatibility
# Begins a conditional branch to check a condition.
if not hasattr(np, "Inf"):
    # Performs a NumPy operation for numerical processing.
    np.Inf = np.inf  # type: ignore

# Begins a block that monitors for exceptions.
try:
    # Imports the module(s) lora as sdr_lora  # type: ignore.
    import lora as sdr_lora  # type: ignore
# Handles a specific exception from the try block.
except Exception as e:
    # Outputs diagnostic or user-facing text.
    print(f"Failed to import external sdr_lora: {e}")
    # Executes the statement `sys.exit(2)`.
    sys.exit(2)


# Defines the function load_cf32.
def load_cf32(path: Path) -> np.ndarray:
    # Executes the statement `data = np.fromfile(path, dtype=np.float32)`.
    data = np.fromfile(path, dtype=np.float32)
    # Begins a conditional branch to check a condition.
    if data.size % 2 != 0:
        # Raises an exception to signal an error.
        raise ValueError(f"File {path} has odd number of float32 elements; expected interleaved I/Q")
    # Executes the statement `i = data[0::2]`.
    i = data[0::2]
    # Executes the statement `q = data[1::2]`.
    q = data[1::2]
    # Returns the computed value to the caller.
    return (i + 1j * q).astype(np.complex64, copy=False)


# Defines the function read_meta.
def read_meta(meta_path: Path) -> Tuple[int, int, int, str]:
    # Executes the statement `meta = json.loads(meta_path.read_text())`.
    meta = json.loads(meta_path.read_text())
    # Executes the statement `sf = int(meta.get("sf"))`.
    sf = int(meta.get("sf"))
    # Executes the statement `bw = int(meta.get("bw"))`.
    bw = int(meta.get("bw"))
    # Executes the statement `fs = int(meta.get("samp_rate") or meta.get("sample_rate") or meta.get("fs"))`.
    fs = int(meta.get("samp_rate") or meta.get("sample_rate") or meta.get("fs"))
    # Executes the statement `payload_hex = meta.get("payload_hex")`.
    payload_hex = meta.get("payload_hex")
    # Returns the computed value to the caller.
    return sf, bw, fs, payload_hex or ""


# Defines the function find_vectors.
def find_vectors(paths: List[Path]) -> List[Tuple[Path, Path]]:
    # Executes the statement `pairs = []`.
    pairs = []
    # Starts a loop iterating over a sequence.
    for base in paths:
        # Begins a conditional branch to check a condition.
        if not base.exists():
            # Skips to the next iteration of the loop.
            continue
        # Starts a loop iterating over a sequence.
        for cf32 in base.glob("**/*.cf32"):
            # Executes the statement `meta = cf32.with_suffix(".json")`.
            meta = cf32.with_suffix(".json")
            # Begins a conditional branch to check a condition.
            if meta.exists():
                # Executes the statement `pairs.append((cf32, meta))`.
                pairs.append((cf32, meta))
    # Returns the computed value to the caller.
    return sorted(pairs)


# Defines the function run_one.
def run_one(cf32: Path, meta: Path, fast: bool = False) -> dict:
    # Executes the statement `sf, bw, fs, exp_hex = read_meta(meta)`.
    sf, bw, fs, exp_hex = read_meta(meta)
    # Executes the statement `meta_all = json.loads(meta.read_text())`.
    meta_all = json.loads(meta.read_text())
    # Executes the statement `samples = load_cf32(cf32)`.
    samples = load_cf32(cf32)
    # Controls for the external decoder behavior
    # Executes the statement `override = {`.
    override = {
        # Executes the statement `'ih': bool(meta_all.get('impl_header', False)),`.
        'ih': bool(meta_all.get('impl_header', False)),
        # Executes the statement `'cr': int(meta_all.get('cr', 2)),`.
        'cr': int(meta_all.get('cr', 2)),
        # Executes the statement `'has_crc': bool(meta_all.get('crc', True)),`.
        'has_crc': bool(meta_all.get('crc', True)),
        # Executes the statement `'length': int(meta_all.get('payload_len', 0)),`.
        'length': int(meta_all.get('payload_len', 0)),
        # Executes the statement `'expected_hex': str(meta_all.get('payload_hex', '') or ''),`.
        'expected_hex': str(meta_all.get('payload_hex', '') or ''),
        # Executes the statement `'ldro_mode': int(meta_all.get('ldro_mode', 0) or 0),`.
        'ldro_mode': int(meta_all.get('ldro_mode', 0) or 0),
        # performance/sweep controls honored by external decoder
        # Executes the statement `'fast': bool(fast),`.
        'fast': bool(fast),
        # try only a small rotation neighborhood in fast mode; otherwise allow full
        # Executes the statement `'sweep_max_rot': 4 if fast else None,  # None => decoder decides (possibly full K)`.
        'sweep_max_rot': 4 if fast else None,  # None => decoder decides (possibly full K)
        # Executes the statement `'try_local': False if fast else True,  # disable heavy local fallback when fast`.
        'try_local': False if fast else True,  # disable heavy local fallback when fast
        # Executes the statement `'quick_local': True,  # ensure local fallback uses quick path when enabled`.
        'quick_local': True,  # ensure local fallback uses quick path when enabled
    # Closes the previously opened dictionary or set literal.
    }
    # Executes the statement `pkts = sdr_lora.decode(samples, sf, bw, fs, override=override)`.
    pkts = sdr_lora.decode(samples, sf, bw, fs, override=override)

    # Defines the function payload_to_hex.
    def payload_to_hex(p):
        # Begins a conditional branch to check a condition.
        if p is None:
            # Returns the computed value to the caller.
            return ""
        # Returns the computed value to the caller.
        return bytes(int(b) for b in p).hex()

    # Executes the statement `out: Dict[str, Any] = {`.
    out: Dict[str, Any] = {
        # Executes the statement `"file": str(cf32),`.
        "file": str(cf32),
        # Executes the statement `"sf": sf,`.
        "sf": sf,
        # Executes the statement `"bw": bw,`.
        "bw": bw,
        # Executes the statement `"fs": fs,`.
        "fs": fs,
        # Executes the statement `"expected": exp_hex,`.
        "expected": exp_hex,
        # Executes the statement `"found": [],`.
        "found": [],
        # Executes the statement `"match": False,`.
        "match": False,
        # Executes the statement `"decoder": "sdr_lora",`.
        "decoder": "sdr_lora",
    # Closes the previously opened dictionary or set literal.
    }
    # Starts a loop iterating over a sequence.
    for pkt in pkts:
        # Executes the statement `cur = {`.
        cur = {
            # Executes the statement `"hdr_ok": int(pkt.hdr_ok),`.
            "hdr_ok": int(pkt.hdr_ok),
            # Executes the statement `"crc_ok": int(pkt.crc_ok),`.
            "crc_ok": int(pkt.crc_ok),
            # Executes the statement `"has_crc": int(pkt.has_crc),`.
            "has_crc": int(pkt.has_crc),
            # Executes the statement `"cr": int(pkt.cr),`.
            "cr": int(pkt.cr),
            # Executes the statement `"ih": int(pkt.ih),`.
            "ih": int(pkt.ih),
            # Executes the statement `"payload_len": (len(pkt.payload) if pkt.payload is not None else 0),`.
            "payload_len": (len(pkt.payload) if pkt.payload is not None else 0),
            # Executes the statement `"hex": payload_to_hex(pkt.payload),`.
            "hex": payload_to_hex(pkt.payload),
        # Closes the previously opened dictionary or set literal.
        }
        # Executes the statement `out["found"].append(cur)`.
        out["found"].append(cur)
        # Begins a conditional branch to check a condition.
        if exp_hex and cur["hex"].lower() == exp_hex.lower():
            # Executes the statement `out["match"] = True`.
            out["match"] = True
    # Old Python receiver fallback removed; we only report sdr_lora results now.
    # Returns the computed value to the caller.
    return out


# Defines the function main.
def main():
    # Executes the statement `ap = argparse.ArgumentParser(description="Batch-decode all cf32 vectors with sdr_lora and summarize")`.
    ap = argparse.ArgumentParser(description="Batch-decode all cf32 vectors with sdr_lora and summarize")
    # Executes the statement `ap.add_argument("--roots", nargs="*", default=[`.
    ap.add_argument("--roots", nargs="*", default=[
        # Executes the statement `"golden_vectors_demo",`.
        "golden_vectors_demo",
        # Executes the statement `"golden_vectors_demo_batch",`.
        "golden_vectors_demo_batch",
        # Executes the statement `"vectors"`.
        "vectors"
    # Executes the statement `], help="Folders to scan for .cf32 + .json pairs")`.
    ], help="Folders to scan for .cf32 + .json pairs")
    # Executes the statement `ap.add_argument("--out", default="results/sdr_lora_batch.json", help="Where to write the full JSON report")`.
    ap.add_argument("--out", default="results/sdr_lora_batch.json", help="Where to write the full JSON report")
    # Executes the statement `ap.add_argument("--fast", action="store_true", help="Enable fast mode: limited sweep and no heavy fallbacks")`.
    ap.add_argument("--fast", action="store_true", help="Enable fast mode: limited sweep and no heavy fallbacks")
    # Executes the statement `args = ap.parse_args()`.
    args = ap.parse_args()

    # Executes the statement `roots = [ROOT / r for r in args.roots]`.
    roots = [ROOT / r for r in args.roots]
    # Executes the statement `pairs = find_vectors(roots)`.
    pairs = find_vectors(roots)
    # Begins a conditional branch to check a condition.
    if not pairs:
        # Outputs diagnostic or user-facing text.
        print("No vectors found")
        # Returns the computed value to the caller.
        return 1
    # Outputs diagnostic or user-facing text.
    print(f"Found {len(pairs)} vectors to decode")

    # Executes the statement `t0 = time.time()`.
    t0 = time.time()
    # Executes the statement `results = []`.
    results = []
    # Executes the statement `ok = 0`.
    ok = 0
    # Starts a loop iterating over a sequence.
    for cf32, meta in pairs:
        # Begins a block that monitors for exceptions.
        try:
            # Executes the statement `res = run_one(cf32, meta, fast=args.fast)`.
            res = run_one(cf32, meta, fast=args.fast)
            # Executes the statement `results.append(res)`.
            results.append(res)
            # Executes the statement `ok += 1 if res.get("match") else 0`.
            ok += 1 if res.get("match") else 0
            # Executes the statement `status = "OK" if res.get("match") else ("MISS" if res.get("found") else "NONE")`.
            status = "OK" if res.get("match") else ("MISS" if res.get("found") else "NONE")
            # Outputs diagnostic or user-facing text.
            print(f"- {cf32.name}: {status}  (exp={res['expected'][:16]}... found={res['found'][0]['hex'][:16]+'...' if res['found'] else ''})")
        # Handles a specific exception from the try block.
        except Exception as e:
            # Executes the statement `results.append({"file": str(cf32), "error": str(e)})`.
            results.append({"file": str(cf32), "error": str(e)})
            # Outputs diagnostic or user-facing text.
            print(f"- {cf32.name}: ERROR {e}")
    # Executes the statement `dt = time.time() - t0`.
    dt = time.time() - t0

    # Write full report
    # Executes the statement `out_path = ROOT / args.out`.
    out_path = ROOT / args.out
    # Executes the statement `out_path.parent.mkdir(parents=True, exist_ok=True)`.
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Executes the statement `out_path.write_text(json.dumps({`.
    out_path.write_text(json.dumps({
        # Executes the statement `"count": len(results),`.
        "count": len(results),
        # Executes the statement `"matched": sum(1 for r in results if r.get("match")),`.
        "matched": sum(1 for r in results if r.get("match")),
        # Executes the statement `"duration_sec": dt,`.
        "duration_sec": dt,
        # Executes the statement `"results": results,`.
        "results": results,
    # Executes the statement `}, indent=2))`.
    }, indent=2))

    # Summary
    # Executes the statement `matched = sum(1 for r in results if r.get("match"))`.
    matched = sum(1 for r in results if r.get("match"))
    # Executes the statement `mism = [r for r in results if (not r.get("match") and not r.get("error") and r.get("found"))]`.
    mism = [r for r in results if (not r.get("match") and not r.get("error") and r.get("found"))]
    # Executes the statement `none = [r for r in results if (not r.get("match") and not r.get("error") and not r.get("found"))]`.
    none = [r for r in results if (not r.get("match") and not r.get("error") and not r.get("found"))]
    # Executes the statement `errs = [r for r in results if r.get("error")]`.
    errs = [r for r in results if r.get("error")]

    # Outputs diagnostic or user-facing text.
    print("\nSummary:")
    # Outputs diagnostic or user-facing text.
    print(f"  Total:   {len(results)}")
    # Outputs diagnostic or user-facing text.
    print(f"  Matched: {matched}")
    # Outputs diagnostic or user-facing text.
    print(f"  Miss:    {len(mism)}")
    # Outputs diagnostic or user-facing text.
    print(f"  None:    {len(none)}")
    # Outputs diagnostic or user-facing text.
    print(f"  Errors:  {len(errs)}")
    # Outputs diagnostic or user-facing text.
    print(f"  Report:  {out_path}")
    # Returns the computed value to the caller.
    return 0 if matched == len(results) else 1


# Begins a conditional branch to check a condition.
if __name__ == "__main__":
    # Raises an exception to signal an error.
    raise SystemExit(main())
