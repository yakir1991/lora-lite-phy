#!/usr/bin/env python3
"""Compare host decoder payloads vs GNU Radio payloads for all sweep captures."""

import json
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Optional

PROJECT_ROOT = Path(__file__).parent.parent
SWEEP_DIR = PROJECT_ROOT / "gr_lora_sdr" / "data" / "generated"
HOST_BINARY = PROJECT_ROOT / "build" / "host_sim" / "lora_replay"
GR_DECODE_SCRIPT = PROJECT_ROOT / "tools" / "gr_decode_capture.py"
LORA_SDR_PY_PATH = PROJECT_ROOT / "gr_lora_sdr" / "install" / "lib" / "python3.12" / "site-packages"
LORA_SDR_LIB_PATH = PROJECT_ROOT / "gr_lora_sdr" / "install" / "lib"


def get_sweep_captures():
    """Find all sweep captures with both .cf32 and .json files."""
    captures = []
    for cf32_file in sorted(SWEEP_DIR.glob("sweep_*.cf32")):
        json_file = cf32_file.with_suffix(".json")
        if json_file.exists():
            captures.append((cf32_file, json_file))
    return captures


def decode_with_host(cf32_path: Path, json_path: Path, payload_out: Path) -> Optional[bytes]:
    """Decode capture using host decoder."""
    cmd = [
        str(HOST_BINARY),
        "--iq", str(cf32_path),
        "--metadata", str(json_path),
        "--dump-payload", str(payload_out),
        "--ignore-ref-mismatch"
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=30)
        if payload_out.exists() and payload_out.stat().st_size > 0:
            return payload_out.read_bytes()
    except Exception as e:
        print(f"  Host decode error: {e}")
    return None


def decode_with_gnuradio(cf32_path: Path, json_path: Path, payload_out: Path) -> Optional[bytes]:
    """Decode capture using GNU Radio."""
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
        result = subprocess.run(cmd, capture_output=True, timeout=60, env=env)
        if payload_out.exists() and payload_out.stat().st_size > 0:
            return payload_out.read_bytes()
    except Exception as e:
        print(f"  GNU Radio decode error: {e}")
    return None


def compare_payloads(host_payload: bytes, gr_payload: bytes) -> tuple[bool, float]:
    """Compare two payloads. Returns (exact_match, similarity_ratio)."""
    if host_payload == gr_payload:
        return True, 1.0
    
    min_len = min(len(host_payload), len(gr_payload))
    max_len = max(len(host_payload), len(gr_payload))
    
    if max_len == 0:
        return True, 1.0
    
    matches = sum(1 for i in range(min_len) if host_payload[i] == gr_payload[i])
    similarity = matches / max_len
    
    return False, similarity


def main():
    captures = get_sweep_captures()
    print(f"Found {len(captures)} sweep captures\n")
    
    results = {
        "total": len(captures),
        "exact_match": 0,
        "partial_match": 0,
        "host_only": 0,
        "gr_only": 0,
        "both_empty": 0,
        "details": []
    }
    
    with tempfile.TemporaryDirectory() as tmpdir:
        for i, (cf32_path, json_path) in enumerate(captures):
            stem = cf32_path.stem
            print(f"[{i+1}/{len(captures)}] {stem}...")
            
            host_payload_file = Path(tmpdir) / f"{stem}_host.bin"
            gr_payload_file = Path(tmpdir) / f"{stem}_gr.bin"
            
            host_payload = decode_with_host(cf32_path, json_path, host_payload_file)
            gr_payload = decode_with_gnuradio(cf32_path, json_path, gr_payload_file)
            
            detail = {
                "capture": stem,
                "host_len": len(host_payload) if host_payload else 0,
                "gr_len": len(gr_payload) if gr_payload else 0
            }
            
            if host_payload and gr_payload:
                exact, similarity = compare_payloads(host_payload, gr_payload)
                if exact:
                    results["exact_match"] += 1
                    detail["status"] = "exact_match"
                    print(f"  ✓ EXACT MATCH ({len(host_payload)} bytes)")
                else:
                    results["partial_match"] += 1
                    detail["status"] = "partial_match"
                    detail["similarity"] = round(similarity * 100, 1)
                    print(f"  ⚠ PARTIAL MATCH ({similarity*100:.1f}% similar, host={len(host_payload)}B, gr={len(gr_payload)}B)")
            elif host_payload:
                results["host_only"] += 1
                detail["status"] = "host_only"
                print(f"  ○ HOST ONLY ({len(host_payload)} bytes)")
            elif gr_payload:
                results["gr_only"] += 1
                detail["status"] = "gr_only"
                print(f"  ○ GR ONLY ({len(gr_payload)} bytes)")
            else:
                results["both_empty"] += 1
                detail["status"] = "both_empty"
                print(f"  ✗ BOTH EMPTY")
            
            results["details"].append(detail)
    
    # Summary
    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)
    print(f"Total captures:     {results['total']}")
    print(f"Exact match:        {results['exact_match']} ({results['exact_match']/results['total']*100:.1f}%)")
    print(f"Partial match:      {results['partial_match']}")
    print(f"Host only:          {results['host_only']}")
    print(f"GNU Radio only:     {results['gr_only']}")
    print(f"Both empty:         {results['both_empty']}")
    
    # Save results
    output_file = PROJECT_ROOT / "docs" / "host_vs_gnuradio_sweep_comparison.json"
    with open(output_file, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to {output_file}")
    
    # List partial matches
    partial = [d for d in results["details"] if d["status"] == "partial_match"]
    if partial:
        print("\nPartial matches:")
        for p in partial:
            print(f"  - {p['capture']}: {p['similarity']}% similar")
    
    return results["exact_match"] == results["total"]


if __name__ == "__main__":
    import sys
    sys.exit(0 if main() else 1)
