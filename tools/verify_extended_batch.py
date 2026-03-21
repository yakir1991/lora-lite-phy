#!/usr/bin/env python3
"""Verify host receiver vs GNU Radio for extended batch."""

from pathlib import Path
import subprocess
import json

REPO_ROOT = Path(__file__).resolve().parent.parent
EXTENDED_DIR = REPO_ROOT / "build" / "receiver_vs_gnuradio_batches" / "extended"
GNU_DIR = REPO_ROOT / "build" / "receiver_vs_gnuradio_regen_gnu_extended"
HOST_SIM = REPO_ROOT / "build" / "host_sim" / "lora_replay"

# Find all cases with GNU payloads
gnu_payloads = sorted(GNU_DIR.glob("*_gnuradio_payload.bin"))

print(f"Found {len(gnu_payloads)} GNU payloads to verify")

passes = []
failures = []

for gnu_payload in gnu_payloads:
    base = gnu_payload.name[:-len("_gnuradio_payload.bin")]
    capture = EXTENDED_DIR / f"{base}.cf32"
    metadata = EXTENDED_DIR / f"{base}.json"
    
    if not capture.exists() or not metadata.exists():
        print(f"SKIP {base}: missing files")
        continue
    
    # Load metadata to check if we need --bypass-crc-verif
    with open(metadata) as f:
        meta = json.load(f)
    
    # Run host receiver
    host_payload = Path(f"/tmp/host_{base}_payload.bin")
    host_payload.unlink(missing_ok=True)
    
    cmd = [
        str(HOST_SIM),
        "--iq", str(capture),
        "--metadata", str(metadata),
        "--dump-payload", str(host_payload),
    ]
    
    # Note: --bypass-crc-verif is not implemented in lora_replay
    # The CRC check happens after decoding regardless of metadata

    print(f"[{gnu_payloads.index(gnu_payload)+1}/{len(gnu_payloads)}] {base}...", end=" ", flush=True)
    
    result = subprocess.run(cmd, capture_output=True, timeout=30)
    
    if result.returncode != 0:
        print(f"✗ FAIL (host rc={result.returncode})")
        failures.append(f"{base}: host failed rc={result.returncode}")
        continue
    
    if not host_payload.exists():
        print(f"✗ FAIL (no host payload)")
        failures.append(f"{base}: no host payload")
        continue
    
    # Compare payloads byte-by-byte
    gnu_bytes = gnu_payload.read_bytes()
    host_bytes = host_payload.read_bytes()
    
    if gnu_bytes == host_bytes:
        print(f"✓ PASS ({len(host_bytes)} bytes)")
        passes.append(base)
    else:
        print(f"✗ FAIL (payload mismatch: host={len(host_bytes)}B gnu={len(gnu_bytes)}B)")
        failures.append(f"{base}: payload mismatch")
        # Show first few differing bytes
        for i, (h, g) in enumerate(zip(host_bytes, gnu_bytes)):
            if h != g:
                failures.append(f"  byte {i}: host={h:02x} gnu={g:02x}")
                if i >= 10:
                    failures.append(f"  ...")
                    break

print(f"\n\n{'='*60}")
print(f"RESULTS: {len(passes)}/{len(gnu_payloads)} PASSED")
if failures:
    print(f"\nFAILURES ({len(failures)} cases):")
    for f in failures:
        print(f"  {f}")
    exit(1)
else:
    print(f"\n✓ All test cases passed strict byte-identical verification!")
