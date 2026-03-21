#!/usr/bin/env python3

"""
Simulated OTA analysis harness (placeholder until hardware captures arrive).
Reads a capture manifest (see docs/captures/README.md) and prints a short summary.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def analyse(manifest: Path) -> dict:
    data = json.loads(manifest.read_text())
    captures = data.get("captures", [])
    return {
        "session_id": data.get("session_id", manifest.parent.name),
        "count": len(captures),
        "spreading_factors": sorted({entry.get("sf") for entry in captures}),
        "bandwidths": sorted({entry.get("bw_hz") for entry in captures}),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="Path to capture_manifest.json")
    parser.add_argument("--output", type=Path, help="Optional JSON summary output")
    args = parser.parse_args()

    summary = analyse(args.manifest.resolve())
    if args.output:
        args.output.write_text(json.dumps(summary, indent=2) + "\n")
    else:
        print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
