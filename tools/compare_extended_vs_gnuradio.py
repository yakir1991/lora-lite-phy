#!/usr/bin/env python3
"""Compare host decoder vs GNU Radio for extended test captures."""

import json
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Optional

PROJECT_ROOT = Path(__file__).parent.parent
DATA_DIR = PROJECT_ROOT / "gr_lora_sdr" / "data" / "generated"
HOST_BINARY = PROJECT_ROOT / "build" / "host_sim" / "lora_replay"
GR_DECODE_SCRIPT = PROJECT_ROOT / "tools" / "gr_decode_capture.py"
LORA_SDR_PY_PATH = PROJECT_ROOT / "gr_lora_sdr" / "install" / "lib" / "python3.12" / "site-packages"
LORA_SDR_LIB_PATH = PROJECT_ROOT / "gr_lora_sdr" / "install" / "lib"

PREFIXES = ["implicit_", "preamble", "payload", "lowbw_", "nocrc_"]


def get_captures():
    """Find all extended test captures."""
    captures = []
    for cf32_file in sorted(DATA_DIR.glob("*.cf32")):
        if any(cf32_file.stem.startswith(p) for p in PREFIXES):
            json_file = cf32_file.with_suffix(".json")
            if json_file.exists():
                captures.append((cf32_file, json_file))
    return captures


def decode_with_host(cf32_path: Path, json_path: Path, payload_out: Path) -> Optional[bytes]:
    cmd = [
        str(HOST_BINARY),
        "--iq", str(cf32_path),
        "--metadata", str(json_path),
        "--dump-payload", str(payload_out),
        "--ignore-ref-mismatch"
    ]
    try:
        subprocess.run(cmd, capture_output=True, timeout=60)
        if payload_out.exists() and payload_out.stat().st_size > 0:
            return payload_out.read_bytes()
    except Exception as e:
        print(f"  Host error: {e}")
    return None


def decode_with_gnuradio(cf32_path: Path, json_path: Path, payload_out: Path) -> Optional[bytes]:
    env = os.environ.copy()
    env["PYTHONPATH"] = f"{LORA_SDR_PY_PATH}:{env.get('PYTHONPATH', '')}"
    env["LD_LIBRARY_PATH"] = f"{LORA_SDR_LIB_PATH}:{env.get('LD_LIBRARY_PATH', '')}"
    
    cmd = [
        "conda", "run", "-n", "gr310", "--no-capture-output",
        "python3", str(GR_DECODE_SCRIPT),
        "--input", str(cf32_path),
        "--metadata", str(json_path),
        "--payload-out", str(payload_out)
    ]
    try:
        subprocess.run(cmd, capture_output=True, timeout=120, env=env)
        if payload_out.exists() and payload_out.stat().st_size > 0:
            return payload_out.read_bytes()
    except Exception as e:
        print(f"  GR error: {e}")
    return None


def main():
    captures = get_captures()
    print(f"Found {len(captures)} extended test captures\n")
    
    results = {"exact_match": 0, "host_only": 0, "gr_only": 0, "both_empty": 0, "partial": 0}
    by_prefix = {p: {"match": 0, "host_only": 0, "gr_empty": 0} for p in PREFIXES}
    
    with tempfile.TemporaryDirectory() as tmpdir:
        for i, (cf32_path, json_path) in enumerate(captures):
            stem = cf32_path.stem
            prefix = next((p for p in PREFIXES if stem.startswith(p)), "other")
            print(f"[{i+1}/{len(captures)}] {stem}...", end=" ")
            
            host_file = Path(tmpdir) / f"{stem}_host.bin"
            gr_file = Path(tmpdir) / f"{stem}_gr.bin"
            
            host = decode_with_host(cf32_path, json_path, host_file)
            gr = decode_with_gnuradio(cf32_path, json_path, gr_file)
            
            if host and gr:
                if host == gr:
                    print(f"✓ EXACT MATCH ({len(host)}B)")
                    results["exact_match"] += 1
                    by_prefix[prefix]["match"] += 1
                else:
                    print(f"⚠ PARTIAL (host={len(host)}B gr={len(gr)}B)")
                    results["partial"] += 1
            elif host:
                print(f"○ HOST ONLY ({len(host)}B)")
                results["host_only"] += 1
                by_prefix[prefix]["host_only"] += 1
            elif gr:
                print(f"○ GR ONLY ({len(gr)}B)")
                results["gr_only"] += 1
            else:
                print("✗ BOTH EMPTY")
                results["both_empty"] += 1
    
    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)
    print(f"Exact match:   {results['exact_match']}")
    print(f"Partial match: {results['partial']}")
    print(f"Host only:     {results['host_only']}")
    print(f"GR only:       {results['gr_only']}")
    print(f"Both empty:    {results['both_empty']}")
    
    print("\nBy category:")
    for p in PREFIXES:
        d = by_prefix[p]
        print(f"  {p:15} match={d['match']:2} host_only={d['host_only']:2}")
    
    return results["partial"] == 0 and results["both_empty"] == 0


if __name__ == "__main__":
    import sys
    sys.exit(0 if main() else 1)
