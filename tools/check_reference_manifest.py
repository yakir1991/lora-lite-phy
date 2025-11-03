#!/usr/bin/env python3
"""
Verify that the reference stage manifest matches the files on disk.

The manifest is generated from `gr_lora_sdr/data/generated/` and stored under
`docs/reference_stage_manifest.json`. This script recomputes the SHA-256 hash of
each capture and stage dump listed in the manifest and ensures the values
haven't drifted. It exits with a non-zero status on the first inconsistency so
CI can flag regressions immediately.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Dict, Iterable, Tuple


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_entry(base_dir: Path, entry: Dict) -> Iterable[Tuple[str, Path]]:
    """Yield (kind, path) pairs for files that failed verification."""
    capture_path = base_dir / entry["capture"]
    expected_capture_hash = entry.get("sha256")
    if expected_capture_hash:
        if not capture_path.exists():
            yield ("missing capture", capture_path)
        else:
            actual_hash = sha256_file(capture_path)
            if actual_hash != expected_capture_hash:
                yield ("capture checksum mismatch", capture_path)

    stage_hashes = entry.get("stage_dump_sha256", {})
    for stage, expected_hash in stage_hashes.items():
        stage_file = base_dir / f"{Path(entry['capture']).stem}_{stage}.txt"
        if not stage_file.exists():
            yield (f"missing stage {stage}", stage_file)
            continue
        actual_stage_hash = sha256_file(stage_file)
        if actual_stage_hash != expected_hash:
            yield (f"stage {stage} checksum mismatch", stage_file)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate reference stage manifest against files on disk."
    )
    parser.add_argument(
        "manifest",
        type=Path,
        help="Path to docs/reference_stage_manifest.json",
    )
    parser.add_argument(
        "--data-dir",
        type=Path,
        default=Path("gr_lora_sdr/data/generated"),
        help="Directory containing the CF32 captures and stage dumps "
        "(default: gr_lora_sdr/data/generated)",
    )
    args = parser.parse_args()

    if not args.manifest.exists():
        print(f"[manifest] missing file: {args.manifest}", file=sys.stderr)
        return 2
    if not args.data_dir.exists():
        print(f"[manifest] data directory missing: {args.data_dir}", file=sys.stderr)
        return 2

    entries = json.loads(args.manifest.read_text())
    failures = []
    for entry in entries:
        for kind, path in verify_entry(args.data_dir, entry):
            failures.append((kind, path))

    if failures:
        print("[manifest] verification FAILED:", file=sys.stderr)
        for kind, path in failures:
            print(f"  - {kind}: {path}", file=sys.stderr)
        return 1

    print(
        f"[manifest] verified {len(entries)} captures under {args.data_dir} "
        f"against {args.manifest}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
