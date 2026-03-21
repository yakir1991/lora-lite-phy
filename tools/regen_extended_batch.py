#!/usr/bin/env python3
"""Regenerate GNU Radio payloads for extended batch using conda environment."""

from pathlib import Path
import subprocess
import json

REPO_ROOT = Path(__file__).resolve().parent.parent
EXTENDED_DIR = REPO_ROOT / "build" / "receiver_vs_gnuradio_batches" / "extended"
OUT_DIR = REPO_ROOT / "build" / "receiver_vs_gnuradio_regen_gnu_extended"
OUT_DIR.mkdir(parents=True, exist_ok=True)

# Find all standalone summary files
summaries = sorted(EXTENDED_DIR.glob("*_standalone_summary.json"))

print(f"Found {len(summaries)} captures to regenerate")

failures = []
for summary in summaries:
    base = summary.name[:-len("_standalone_summary.json")]
    capture = EXTENDED_DIR / f"{base}.cf32"
    
    if not capture.exists():
        print(f"SKIP {base}: capture not found")
        continue
    
    # Load metadata
    with open(summary) as f:
        summary_data = json.load(f)
    meta = summary_data.get("metadata", {})
    
    # Write metadata file for gr_decode_capture
    meta_out = OUT_DIR / f"{base}.meta.json"
    with open(meta_out, "w") as f:
        json.dump(meta, f, indent=2)
    
    payload_out = OUT_DIR / f"{base}_gnuradio_payload.bin"
    
    # Build command with conda
    cmd = [
        "conda", "run", "-n", "gr310",
        "python", str(REPO_ROOT / "tools" / "gr_decode_capture.py"),
        "--input", str(capture),
        "--metadata", str(meta_out),
        "--payload-out", str(payload_out),
        "--timeout-s", "120",
        "--force-manual"
    ]
    
    # Add bypass-crc-verif for implicit header no-crc cases
    if not bool(meta.get("has_crc", True)):
        cmd.append("--bypass-crc-verif")
    
    print(f"[{summaries.index(summary)+1}/{len(summaries)}] {base}...", end=" ", flush=True)
    
    # Set up environment
    env = {
        "PYTHONPATH": f"{REPO_ROOT}/gr_lora_sdr/install/lib/python3.12/site-packages:{REPO_ROOT}/gr_lora_sdr/python",
        "LD_LIBRARY_PATH": f"{REPO_ROOT}/gr_lora_sdr/install/lib",
    }
    
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=130, env={**subprocess.os.environ, **env})
    
    if result.returncode != 0:
        print(f"✗ FAIL")
        failures.append(f"{base}: rc={result.returncode} stderr={result.stderr[-200:]}")
    else:
        # Check if payload was generated
        if payload_out.exists() and payload_out.stat().st_size > 0:
            print(f"✓ OK ({payload_out.stat().st_size} bytes)")
        else:
            print(f"✗ FAIL (no payload)")
            failures.append(f"{base}: no payload generated")

if failures:
    print(f"\n\nFAILURES ({len(failures)}/{len(summaries)}):")
    for f in failures:
        print(f"  {f}")
    exit(1)
else:
    print(f"\n✓ All {len(summaries)} captures regenerated successfully")
    print(f"Output: {OUT_DIR}")
