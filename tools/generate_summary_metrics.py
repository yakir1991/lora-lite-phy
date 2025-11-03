#!/usr/bin/env python3

"""Generate lora_replay summary JSON files for every capture in the manifest."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path


def run_lora_replay(binary: Path, capture: Path, summary: Path) -> None:
    summary.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(binary), "--iq", str(capture), "--summary", str(summary)],
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"lora_replay failed for {capture} (exit {result.returncode})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="Path to reference manifest JSON")
    parser.add_argument("data_dir", type=Path, help="Directory containing reference captures")
    parser.add_argument("output_dir", type=Path, help="Directory where summaries should be written")
    parser.add_argument(
        "--lora-replay",
        type=Path,
        default=Path("host_sim/lora_replay"),
        help="Path to the lora_replay executable (default: host_sim/lora_replay)",
    )
    args = parser.parse_args()

    manifest_entries = json.loads(args.manifest.read_text())
    binary = args.lora_replay.resolve()
    if not binary.exists():
        raise FileNotFoundError(f"lora_replay executable not found: {binary}")

    for entry in manifest_entries:
        capture_name = entry["capture"]
        capture_path = (args.data_dir / capture_name).resolve()
        if not capture_path.exists():
            raise FileNotFoundError(f"Capture missing: {capture_path}")
        summary_path = args.output_dir / capture_name.replace(".cf32", ".json")
        print(f"[summary] {capture_name} -> {summary_path}")
        run_lora_replay(binary, capture_path, summary_path)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

