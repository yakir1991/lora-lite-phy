#!/usr/bin/env python3
"""
Regenerate stage dump text files for every capture listed in
docs/reference_stage_manifest.json and refresh the manifest hashes.

The script runs the already-built host_sim/lora_replay binary with
--dump-stages pointing at the capture basename inside gr_lora_sdr/data/generated.
"""

import argparse
import json
import subprocess
import hashlib
from pathlib import Path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def regenerate_stage_dumps(lora_replay: Path, data_dir: Path, manifest: Path) -> None:
    entries = json.loads(manifest.read_text())
    for entry in entries:
        capture = entry["capture"]
        capture_path = data_dir / capture
        if not capture_path.exists():
            raise FileNotFoundError(f"Capture file missing: {capture_path}")
        meta_path = capture_path.with_suffix(".json")
        if not meta_path.exists():
            raise FileNotFoundError(f"Metadata file missing: {meta_path}")
        base = data_dir / capture_path.stem
        print(f"Generating stage dumps for {capture} -> {base}_*.txt")
        cmd = [
            str(lora_replay),
            "--iq",
            str(capture_path),
            "--metadata",
            str(meta_path),
            "--dump-stages",
            str(base),
        ]
        subprocess.run(cmd, check=True)
        stage_sha = {}
        for suffix in ("fft", "gray", "deinterleaver", "hamming"):
            path = Path(f"{base}_{suffix}.txt")
            stage_sha[suffix] = sha256_file(path)
        entry["stage_dump_sha256"] = stage_sha
    manifest.write_text(json.dumps(entries, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser(description="Update stage dumps and manifest hashes")
    parser.add_argument("--lora-replay", default="build/host_sim/lora_replay", type=Path)
    parser.add_argument("--data-dir", default="gr_lora_sdr/data/generated", type=Path)
    parser.add_argument("--manifest", default="docs/reference_stage_manifest.json", type=Path)
    args = parser.parse_args()

    regenerate_stage_dumps(args.lora_replay.resolve(), args.data_dir.resolve(), args.manifest.resolve())


if __name__ == "__main__":
    main()
