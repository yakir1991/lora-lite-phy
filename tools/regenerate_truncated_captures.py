#!/usr/bin/env python3
"""
Regenerate truncated LoRa captures that are too short for full decoding.

This script identifies captures where the file is shorter than needed for
complete payload + CRC decoding, and regenerates them with proper padding.

Usage:
    python tools/regenerate_truncated_captures.py [--dry-run] [--only NAME]
"""

import argparse
import json
import math
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GENERATED_DIR = REPO_ROOT / "gr_lora_sdr" / "data" / "generated"


def estimate_required_samples(metadata: dict) -> int:
    """Estimate the minimum number of samples needed for full decode."""
    sf = int(metadata["sf"])
    bw = int(metadata["bw"])
    sample_rate = int(metadata["sample_rate"])
    cr = int(metadata["cr"])
    payload_len = int(metadata.get("payload_len", 32))
    preamble_len = int(metadata.get("preamble_len", 8))
    has_crc = bool(metadata.get("has_crc", True))
    implicit_header = bool(metadata.get("implicit_header", False))
    ldro = bool(metadata.get("ldro", 0))

    # Calculate symbol duration
    samples_per_symbol = (2**sf) * (sample_rate / bw)

    # Preamble symbols (Npreamble + 4.25)
    preamble_symbols = preamble_len + 4.25

    # Payload symbol calculation (Semtech formula)
    de = 1 if ldro else 0
    ih = 1 if implicit_header else 0
    crc_bits = 1 if has_crc else 0

    denom = 4.0 * (sf - 2 * de)
    if denom <= 0:
        denom = 1.0
    numer = 8.0 * payload_len - 4.0 * sf + 28.0 + 16.0 * crc_bits - 20.0 * ih
    payload_symbols = 8.0
    if numer > 0:
        payload_symbols += math.ceil(numer / denom) * (cr + 4)

    total_symbols = preamble_symbols + payload_symbols

    # Add margin for alignment and tail
    margin = 1.5  # 50% extra for safety
    tail_guard = 4  # Extra symbols at end

    required_samples = int(math.ceil((total_symbols + tail_guard) * samples_per_symbol * margin))
    return required_samples


def check_capture(cf32_path: Path) -> dict:
    """Check if a capture file is truncated."""
    json_path = cf32_path.with_suffix(".json")
    if not json_path.exists():
        return {"status": "no_metadata", "path": str(cf32_path)}

    with open(json_path) as f:
        metadata = json.load(f)

    file_size = cf32_path.stat().st_size
    actual_samples = file_size // 8  # complex float32 = 8 bytes

    required_samples = estimate_required_samples(metadata)

    is_truncated = actual_samples < required_samples * 0.65  # 65% threshold (conservative)

    return {
        "status": "truncated" if is_truncated else "ok",
        "path": str(cf32_path),
        "name": cf32_path.stem,
        "actual_samples": actual_samples,
        "required_samples": required_samples,
        "metadata": metadata,
        "shortfall_percent": max(0, (required_samples - actual_samples) / required_samples * 100),
    }


def regenerate_capture(name: str, metadata: dict, output_dir: Path, dry_run: bool = False) -> bool:
    """Regenerate a single capture using GNU Radio."""
    required_samples = estimate_required_samples(metadata)

    # Build the command for tx_rx_simulation.py
    tx_rx_script = REPO_ROOT / "gr_lora_sdr" / "examples" / "tx_rx_simulation.py"
    if not tx_rx_script.exists():
        print(f"  ERROR: tx_rx_simulation.py not found at {tx_rx_script}")
        return False

    payload_path = REPO_ROOT / "build" / f"payload_{name}.bin"

    # Create payload file if needed
    payload_len = metadata.get("payload_len", 32)
    if not payload_path.exists() or payload_path.stat().st_size != payload_len:
        if not dry_run:
            payload_path.parent.mkdir(parents=True, exist_ok=True)
            # Generate deterministic payload based on name
            import hashlib
            seed = int(hashlib.md5(name.encode()).hexdigest()[:8], 16)
            import random
            random.seed(seed)
            payload_bytes = bytes([random.randint(0, 255) for _ in range(payload_len)])
            payload_path.write_bytes(payload_bytes)

    output_cf32 = output_dir / f"{name}.cf32"
    output_json = output_dir / f"{name}.json"

    cmd = [
        "conda", "run", "-n", "gr310", "python", str(tx_rx_script),
        "--sf", str(metadata["sf"]),
        "--bw", str(metadata["bw"]),
        "--cr", str(metadata["cr"]),
        "--samp-rate", str(metadata["sample_rate"]),
        "--payload", str(payload_path),
        "--output", str(output_cf32),
        "--capture-samples", str(required_samples),
    ]

    if metadata.get("implicit_header"):
        cmd.append("--implicit-header")
    if not metadata.get("has_crc", True):
        cmd.append("--no-crc")
    if metadata.get("ldro"):
        cmd.append("--ldro")

    print(f"  Command: {' '.join(cmd[:6])}...")

    if dry_run:
        print(f"  [DRY RUN] Would regenerate with {required_samples} samples")
        return True

    try:
        result = subprocess.run(
            cmd,
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            timeout=300,
        )
        if result.returncode != 0:
            print(f"  ERROR: {result.stderr[:200]}")
            return False

        # Verify the new file
        if output_cf32.exists():
            new_size = output_cf32.stat().st_size // 8
            print(f"  SUCCESS: Generated {new_size} samples (was {metadata.get('sample_count', 'unknown')})")

            # Update metadata JSON
            new_metadata = metadata.copy()
            new_metadata["sample_count"] = new_size
            new_metadata["duration_secs"] = new_size / metadata["sample_rate"]
            output_json.write_text(json.dumps(new_metadata, indent=2) + "\n")
            return True
        else:
            print(f"  ERROR: Output file not created")
            return False

    except subprocess.TimeoutExpired:
        print(f"  ERROR: Command timed out")
        return False
    except Exception as e:
        print(f"  ERROR: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(description="Regenerate truncated LoRa captures")
    parser.add_argument("--dry-run", action="store_true", help="Show what would be done without doing it")
    parser.add_argument("--only", type=str, help="Only process captures matching this name")
    parser.add_argument("--output-dir", type=Path, default=GENERATED_DIR, help="Output directory")
    args = parser.parse_args()

    print("Scanning for truncated captures...")
    print(f"  Directory: {GENERATED_DIR}")
    print()

    truncated = []
    for cf32_path in sorted(GENERATED_DIR.glob("*.cf32")):
        if args.only and args.only not in cf32_path.stem:
            continue

        result = check_capture(cf32_path)
        if result["status"] == "truncated":
            truncated.append(result)
            print(f"TRUNCATED: {result['name']}")
            print(f"  Actual: {result['actual_samples']:,} samples")
            print(f"  Required: {result['required_samples']:,} samples")
            print(f"  Shortfall: {result['shortfall_percent']:.1f}%")
            print()

    if not truncated:
        print("No truncated captures found!")
        return 0

    print(f"\nFound {len(truncated)} truncated capture(s)")
    print()

    if args.dry_run:
        print("[DRY RUN MODE - no changes will be made]")
        print()

    success_count = 0
    for item in truncated:
        print(f"Regenerating: {item['name']}")
        if regenerate_capture(item["name"], item["metadata"], args.output_dir, args.dry_run):
            success_count += 1
        print()

    print(f"\nSummary: {success_count}/{len(truncated)} captures regenerated successfully")

    if not args.dry_run and success_count < len(truncated):
        print("\nNote: Some captures failed to regenerate. This may be due to:")
        print("  - Missing GNU Radio environment (conda env 'gr310')")
        print("  - Missing tx_rx_simulation.py script")
        print("  - Other environment issues")
        print("\nTo regenerate manually, ensure gr-lora_sdr is installed and run:")
        print("  conda activate gr310")
        print("  python gr_lora_sdr/examples/tx_rx_simulation.py --help")

    return 0 if success_count == len(truncated) else 1


if __name__ == "__main__":
    sys.exit(main())
