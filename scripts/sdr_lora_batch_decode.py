#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import sys
import time
from typing import List, Tuple, Optional, Dict, Any

import numpy as np

# Allow importing external/sdr_lora
ROOT = Path(__file__).resolve().parents[1]
SDR_LORA = ROOT / "external" / "sdr_lora"
if str(SDR_LORA) not in sys.path:
    sys.path.insert(0, str(SDR_LORA))
# Add project root for importing our receiver
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

# NumPy 2.0 compatibility
if not hasattr(np, "Inf"):
    np.Inf = np.inf  # type: ignore

try:
    import lora as sdr_lora  # type: ignore
except Exception as e:
    print(f"Failed to import external sdr_lora: {e}")
    sys.exit(2)


def load_cf32(path: Path) -> np.ndarray:
    data = np.fromfile(path, dtype=np.float32)
    if data.size % 2 != 0:
        raise ValueError(f"File {path} has odd number of float32 elements; expected interleaved I/Q")
    i = data[0::2]
    q = data[1::2]
    return (i + 1j * q).astype(np.complex64, copy=False)


def read_meta(meta_path: Path) -> Tuple[int, int, int, str]:
    meta = json.loads(meta_path.read_text())
    sf = int(meta.get("sf"))
    bw = int(meta.get("bw"))
    fs = int(meta.get("samp_rate") or meta.get("sample_rate") or meta.get("fs"))
    payload_hex = meta.get("payload_hex")
    return sf, bw, fs, payload_hex or ""


def find_vectors(paths: List[Path]) -> List[Tuple[Path, Path]]:
    pairs = []
    for base in paths:
        if not base.exists():
            continue
        for cf32 in base.glob("**/*.cf32"):
            meta = cf32.with_suffix(".json")
            if meta.exists():
                pairs.append((cf32, meta))
    return sorted(pairs)


def run_one(cf32: Path, meta: Path, fast: bool = False) -> dict:
    sf, bw, fs, exp_hex = read_meta(meta)
    meta_all = json.loads(meta.read_text())
    samples = load_cf32(cf32)
    # Controls for the external decoder behavior
    override = {
        'ih': bool(meta_all.get('impl_header', False)),
        'cr': int(meta_all.get('cr', 2)),
        'has_crc': bool(meta_all.get('crc', True)),
        'length': int(meta_all.get('payload_len', 0)),
        'expected_hex': str(meta_all.get('payload_hex', '') or ''),
        'ldro_mode': int(meta_all.get('ldro_mode', 0) or 0),
        # performance/sweep controls honored by external decoder
        'fast': bool(fast),
        # try only a small rotation neighborhood in fast mode; otherwise allow full
        'sweep_max_rot': 4 if fast else None,  # None => decoder decides (possibly full K)
        'try_local': False if fast else True,  # disable heavy local fallback when fast
        'quick_local': True,  # ensure local fallback uses quick path when enabled
    }
    pkts = sdr_lora.decode(samples, sf, bw, fs, override=override)

    def payload_to_hex(p):
        if p is None:
            return ""
        return bytes(int(b) for b in p).hex()

    out: Dict[str, Any] = {
        "file": str(cf32),
        "sf": sf,
        "bw": bw,
        "fs": fs,
        "expected": exp_hex,
        "found": [],
        "match": False,
        "decoder": "sdr_lora",
    }
    for pkt in pkts:
        cur = {
            "hdr_ok": int(pkt.hdr_ok),
            "crc_ok": int(pkt.crc_ok),
            "has_crc": int(pkt.has_crc),
            "cr": int(pkt.cr),
            "ih": int(pkt.ih),
            "payload_len": (len(pkt.payload) if pkt.payload is not None else 0),
            "hex": payload_to_hex(pkt.payload),
        }
        out["found"].append(cur)
        if exp_hex and cur["hex"].lower() == exp_hex.lower():
            out["match"] = True
    # Old Python receiver fallback removed; we only report sdr_lora results now.
    return out


def main():
    ap = argparse.ArgumentParser(description="Batch-decode all cf32 vectors with sdr_lora and summarize")
    ap.add_argument("--roots", nargs="*", default=[
        "golden_vectors_demo",
        "golden_vectors_demo_batch",
        "vectors"
    ], help="Folders to scan for .cf32 + .json pairs")
    ap.add_argument("--out", default="stage_dump/sdr_lora_batch.json", help="Where to write the full JSON report")
    ap.add_argument("--fast", action="store_true", help="Enable fast mode: limited sweep and no heavy fallbacks")
    args = ap.parse_args()

    roots = [ROOT / r for r in args.roots]
    pairs = find_vectors(roots)
    if not pairs:
        print("No vectors found")
        return 1
    print(f"Found {len(pairs)} vectors to decode")

    t0 = time.time()
    results = []
    ok = 0
    for cf32, meta in pairs:
        try:
            res = run_one(cf32, meta, fast=args.fast)
            results.append(res)
            ok += 1 if res.get("match") else 0
            status = "OK" if res.get("match") else ("MISS" if res.get("found") else "NONE")
            print(f"- {cf32.name}: {status}  (exp={res['expected'][:16]}... found={res['found'][0]['hex'][:16]+'...' if res['found'] else ''})")
        except Exception as e:
            results.append({"file": str(cf32), "error": str(e)})
            print(f"- {cf32.name}: ERROR {e}")
    dt = time.time() - t0

    # Write full report
    out_path = ROOT / args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({
        "count": len(results),
        "matched": sum(1 for r in results if r.get("match")),
        "duration_sec": dt,
        "results": results,
    }, indent=2))

    # Summary
    matched = sum(1 for r in results if r.get("match"))
    mism = [r for r in results if (not r.get("match") and not r.get("error") and r.get("found"))]
    none = [r for r in results if (not r.get("match") and not r.get("error") and not r.get("found"))]
    errs = [r for r in results if r.get("error")]

    print("\nSummary:")
    print(f"  Total:   {len(results)}")
    print(f"  Matched: {matched}")
    print(f"  Miss:    {len(mism)}")
    print(f"  None:    {len(none)}")
    print(f"  Errors:  {len(errs)}")
    print(f"  Report:  {out_path}")
    return 0 if matched == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
