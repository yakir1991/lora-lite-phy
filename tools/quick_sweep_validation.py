#!/usr/bin/env python3
"""Quick sweep validation script."""

import subprocess
import os
from pathlib import Path

BUILD_DIR = Path(__file__).resolve().parent.parent / "build"
GENERATED_DIR = Path(__file__).resolve().parent.parent / "gr_lora_sdr" / "data" / "generated"

pass_count = 0
fail_count = 0
skip_count = 0
failures = []

for cf32 in sorted(GENERATED_DIR.glob("sweep_*.cf32")):
    json_f = cf32.with_suffix(".json")
    if not json_f.exists():
        continue
    
    result = subprocess.run(
        [str(BUILD_DIR / "host_sim" / "lora_replay"), "--iq", str(cf32), "--metadata", str(json_f)],
        capture_output=True, timeout=120
    )
    output = result.stdout.decode('utf-8', errors='replace') + result.stderr.decode('utf-8', errors='replace')
    
    name = cf32.stem
    if "CRC decoded" in output and "OK" in output:
        pass_count += 1
    elif "skipping CRC" in output:
        skip_count += 1
        failures.append((name, "SKIP"))
    else:
        fail_count += 1
        failures.append((name, "FAIL"))

total = pass_count + fail_count + skip_count
print(f"Sweep Validation Results:")
print(f"  PASS: {pass_count}/{total}")
print(f"  SKIP: {skip_count}/{total}")
print(f"  FAIL: {fail_count}/{total}")

if failures:
    print("\nNon-passing captures:")
    for name, status in failures:
        print(f"  {name}: {status}")
