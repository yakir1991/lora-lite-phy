#!/usr/bin/env python3

"""Generate summary metrics and compare them against the recorded baseline."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def run(cmd: list[str]) -> None:
    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"Command failed ({result.returncode}): {' '.join(cmd)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("data_dir", type=Path)
    parser.add_argument("summary_dir", type=Path)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("--lora-replay", type=Path, default=Path("host_sim/lora_replay"))
    parser.add_argument("--python", default="python3")
    args = parser.parse_args()

    generate_cmd = [
        args.python,
        str(Path(__file__).parent / "generate_summary_metrics.py"),
        str(args.manifest),
        str(args.data_dir),
        str(args.summary_dir),
        "--lora-replay",
        str(args.lora_replay),
    ]
    compare_cmd = [
        args.python,
        str(Path(__file__).parent / "compare_summary_metrics.py"),
        str(args.baseline),
        str(args.summary_dir),
    ]

    run(generate_cmd)
    run(compare_cmd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

