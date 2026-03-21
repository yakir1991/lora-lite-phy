#!/usr/bin/env python3

"""
Run interoperability comparison between lora_replay and the GNU Radio gr-lora_sdr chain.
Produces a JSON summary of payload/CRC mismatches per capture.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, Optional, Tuple
import os

REPO_ROOT = Path(__file__).resolve().parent.parent


@dataclass
class InteropResult:
    capture: str
    reference_payload: Path
    standalone_payload: Path
    payload_mismatches: int
    crc_mismatch: bool


def _gr_python_path() -> Optional[str]:
    install_dir = REPO_ROOT / "gr_lora_sdr/install/lib"
    if not install_dir.exists():
        return None
    candidates = sorted(install_dir.glob("python*/site-packages"))
    if not candidates:
        return None
    return str(candidates[0].resolve())


def run_lora_replay(lora_replay: Path, capture: Path, metadata: Path, out_dir: Path) -> Path:
    payload_out = out_dir / "standalone_payload.bin"
    summary_file = out_dir / "standalone_summary.json"

    cmd = [
        str(lora_replay),
        "--iq",
        str(capture),
        "--metadata",
        str(metadata),
        "--summary",
        str(summary_file),
        "--dump-payload",
        str(payload_out),
    ]
    env = os.environ.copy()
    result = subprocess.run(cmd, check=False, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"lora_replay failed ({result.returncode}): {' '.join(cmd)}")
    return payload_out


def compare_payloads(ref_path: Path, test_path: Path) -> int:
    ref_bytes = ref_path.read_bytes()
    test_bytes = test_path.read_bytes()
    max_len = max(len(ref_bytes), len(test_bytes))
    mismatches = 0
    for idx in range(max_len):
        ref_byte = ref_bytes[idx] if idx < len(ref_bytes) else None
        test_byte = test_bytes[idx] if idx < len(test_bytes) else None
        if ref_byte != test_byte:
            mismatches += 1
    return mismatches


def compare_crc(payload_len: int, ref_bytes: bytes, test_bytes: bytes) -> bool:
    ref_payload = ref_bytes[:payload_len]
    test_payload = test_bytes[:payload_len]
    return compute_crc_py(ref_payload) != compute_crc_py(test_payload)


def compute_crc_py(payload: bytes) -> int:
    crc = 0x0000
    for byte in payload:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def run_interop(
    gnuradio_python: Iterable[str],
    lora_replay: Path,
    capture: Path,
    metadata_dir: Path,
    work_root: Path,
) -> InteropResult:
    metadata_path = metadata_dir / (capture.stem + ".json")
    if not metadata_path.exists():
        raise FileNotFoundError(f"Metadata not found for capture {capture.name}: {metadata_path}")
    metadata = json.loads(metadata_path.read_text())

    with tempfile.TemporaryDirectory(dir=work_root) as tmp:
        tmp_dir = Path(tmp)
        gnuradio_payload = tmp_dir / "gnuradio_payload.bin"
        cmd = list(gnuradio_python) + [
            str((REPO_ROOT / "tools/gr_decode_capture.py").resolve()),
            "--input",
            str(capture),
            "--metadata",
            str(metadata_path),
            "--payload-out",
            str(gnuradio_payload),
        ]
        env = os.environ.copy()
        paths = []
        gr_python = _gr_python_path()
        if gr_python:
            paths.append(gr_python)
        gr_python_src = (REPO_ROOT / "gr_lora_sdr/python").resolve()
        if gr_python_src.exists():
            paths.append(str(gr_python_src))
        existing_py = env.get("PYTHONPATH")
        if existing_py:
            paths.append(existing_py)
        if paths:
            env["PYTHONPATH"] = ":".join(paths)
        gr_lib = (REPO_ROOT / "gr_lora_sdr/install/lib").resolve()
        if gr_lib.exists():
            existing_ld = env.get("LD_LIBRARY_PATH")
            env["LD_LIBRARY_PATH"] = f"{gr_lib}:{existing_ld}" if existing_ld else str(gr_lib)
        result = subprocess.run(cmd, check=False, env=env)
        if result.returncode != 0:
            raise RuntimeError(f"GNU Radio decode failed ({result.returncode}): {' '.join(cmd)}")

        standalone_payload = run_lora_replay(lora_replay, capture, metadata_path, tmp_dir)

        payload_mismatches = compare_payloads(gnuradio_payload, standalone_payload)
        ref_bytes = Path(gnuradio_payload).read_bytes()
        test_bytes = Path(standalone_payload).read_bytes()
        crc_mismatch = compare_crc(metadata["payload_len"], ref_bytes, test_bytes)

        result_dir = work_root / capture.stem
        result_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(gnuradio_payload, result_dir / "gnuradio_payload.bin")
        shutil.copy2(standalone_payload, result_dir / "standalone_payload.bin")

        return InteropResult(
            capture=capture.name,
            reference_payload=result_dir / "gnuradio_payload.bin",
            standalone_payload=result_dir / "standalone_payload.bin",
            payload_mismatches=payload_mismatches,
            crc_mismatch=crc_mismatch,
        )


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="JSON manifest of captures (reference_stage_manifest.json)")
    parser.add_argument("data_dir", type=Path, help="Directory containing capture .cf32/.json pairs")
    parser.add_argument("output", type=Path, help="Interop comparison JSON")
    parser.add_argument("--gnuradio-python", default="python3", help="Python executable with GNU Radio modules")
    parser.add_argument("--gnuradio-env", default="", help="Optional conda environment name providing GNU Radio")
    parser.add_argument("--lora-replay", type=Path, default=Path("host_sim/lora_replay"))
    parser.add_argument("--captures", nargs="*", help="Subset of captures to test")
    parser.add_argument("--capture-dir", type=Path, help="Directory containing cf32 captures (metadata with same stem)")
    parser.add_argument("--work-dir", type=Path, default=Path("build/interp_compare"))
    args = parser.parse_args(argv)

    gnuradio_cmd = [args.gnuradio_python]
    if args.gnuradio_env:
        gnuradio_cmd = ["conda", "run", "-n", args.gnuradio_env, args.gnuradio_python]

    lora_replay = args.lora_replay.resolve()
    if not lora_replay.exists():
        raise FileNotFoundError(f"lora_replay not found: {lora_replay}")

    manifest_path = args.manifest.resolve()
    data_dir = args.data_dir.resolve()
    manifest = json.loads(manifest_path.read_text())
    capture_filter = set(args.captures or [])
    capture_entries: dict[Path, tuple[str, Path, Path]] = {}

    for entry in manifest:
        capture_name = entry["capture"]
        if capture_filter and capture_name not in capture_filter:
            continue
        capture_path = data_dir / capture_name
        metadata_path = capture_path.with_suffix(".json")
        capture_entries[capture_path.resolve()] = (capture_name, capture_path, metadata_path)

    if args.capture_dir:
        capture_dir = args.capture_dir.resolve()
        for cf32_path in sorted(capture_dir.glob("*.cf32")):
            capture_name = cf32_path.name
            if capture_filter and capture_name not in capture_filter:
                continue
            metadata_path = cf32_path.with_suffix(".json")
            capture_entries[cf32_path.resolve()] = (capture_name, cf32_path, metadata_path)

    results = []

    work_dir = args.work_dir.resolve()
    work_dir.mkdir(parents=True, exist_ok=True)

    for capture_name, capture_path, metadata_path in capture_entries.values():
        if not capture_path.exists():
            raise FileNotFoundError(f"Capture missing: {capture_path}")

        result = run_interop(
            gnuradio_python=gnuradio_cmd,
            lora_replay=lora_replay,
            capture=capture_path,
            metadata_dir=metadata_path.parent,
            work_root=work_dir,
        )
        results.append(
            {
                "capture": result.capture,
                "payload_mismatches": result.payload_mismatches,
                "crc_mismatch": result.crc_mismatch,
                "gnuradio_payload": str(result.reference_payload),
                "standalone_payload": str(result.standalone_payload),
            }
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
