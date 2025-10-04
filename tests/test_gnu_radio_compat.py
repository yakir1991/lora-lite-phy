#!/usr/bin/env python3
"""GNU Radio vs. C++ receiver compatibility smoke test.

The project now relies on the vendored GNU Radio implementation under
``external/sdr_lora`` as the Python reference. This test verifies that the
native C++ receiver (`cpp_receiver`) produces the same payload as the GNU Radio
decoder for a small sample of demo vectors.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import pytest


GR_SCRIPT = Path("external/gr_lora_sdr/scripts/decode_offline_recording.py")
CPP_BINARY_CANDIDATES = (
    Path("cpp_receiver/build/decode_cli"),
    Path("cpp_receiver/build/Release/decode_cli"),
    Path("cpp_receiver/build/Debug/decode_cli"),
    Path("cpp_receiver/build/decode_cli.exe"),  # Windows
)


def _have_gnuradio_script() -> bool:
    return GR_SCRIPT.exists()


def _resolve_cpp_binary() -> Optional[Path]:
    for candidate in CPP_BINARY_CANDIDATES:
        if candidate.exists():
            return candidate
    return None


def _collect_vector_pairs(vectors_dir: Path) -> List[Tuple[Path, Path]]:
    pairs: List[Tuple[Path, Path]] = []
    if not vectors_dir.exists():
        return pairs
    for cf32_file in sorted(vectors_dir.glob("*.cf32")):
        json_file = cf32_file.with_suffix(".json")
        if json_file.exists():
            pairs.append((cf32_file, json_file))
    return pairs


def run_gnu_radio_decoder(vector_path: Path, metadata: Dict) -> Dict:
    """Run the GNU Radio decoder (Python reference)."""

    cmd = [
        sys.executable,
        str(GR_SCRIPT),
        str(vector_path),
        "--sf", str(metadata["sf"]),
        "--bw", str(metadata["bw"]),
        "--samp-rate", str(metadata["samp_rate"]),
        "--cr", str(metadata["cr"]),
        "--ldro-mode", str(metadata["ldro_mode"]),
        "--format", "cf32",
    ]
    cmd.append("--has-crc" if metadata.get("crc", True) else "--no-crc")
    cmd.append("--impl-header" if metadata.get("impl_header") else "--explicit-header")

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return {"status": "timeout", "stdout": "", "stderr": ""}

    if result.returncode != 0:
        return {
            "status": "failed",
            "stdout": result.stdout,
            "stderr": result.stderr,
            "frames": [],
        }

    frames: List[Dict[str, str]] = []
    for line in result.stdout.strip().splitlines():
        line = line.strip()
        if line.startswith("Frame") and ":" in line:
            frames.append({"info": line.split(":", 1)[1].strip()})
        elif line.startswith("Hex:") and frames:
            frames[-1]["hex"] = line.replace("Hex:", "").strip()
        elif line.startswith("Text:") and frames:
            frames[-1]["text"] = line.replace("Text:", "").strip()

    return {
        "status": "success",
        "stdout": result.stdout,
        "stderr": result.stderr,
        "frames": frames,
    }


def run_lora_lite_cpp(vector_path: Path, metadata: Dict) -> Dict:
    """Run the native C++ receiver (decode_cli)."""

    binary = _resolve_cpp_binary()
    if not binary:
        return {"status": "skipped", "error": "decode_cli binary not found", "payload_hex": None}

    cmd = [
        str(binary),
        "--sf", str(metadata["sf"]),
        "--bw", str(metadata["bw"]),
        "--fs", str(metadata["samp_rate"]),
        "--ldro", "1" if metadata.get("ldro_mode") else "0",
        str(vector_path),
    ]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return {"status": "timeout", "payload_hex": None, "stdout": "", "stderr": ""}

    payload_hex: Optional[str] = None
    for line in result.stdout.splitlines():
        if line.startswith("payload_hex="):
            payload_hex = line.split("=", 1)[1].strip()
            break

    status = "success" if result.returncode == 0 else "failed"
    return {
        "status": status,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "payload_hex": payload_hex,
        "binary": str(binary),
    }


def _extract_gr_payload_hex(result: Dict) -> Optional[str]:
    if not result or result.get("status") != "success":
        return None
    for frame in result.get("frames", []):
        hex_value = frame.get("hex")
        if hex_value:
            return hex_value.replace(" ", "").lower()
    return None


@pytest.mark.skipif(not _have_gnuradio_script(), reason="GNU Radio offline decoder not available")
def test_cpp_receiver_matches_gnu_radio():
    vectors_dir = Path("golden_vectors_demo_batch")
    pairs = _collect_vector_pairs(vectors_dir)
    if not pairs:
        pytest.skip("no vectors found in golden_vectors_demo_batch")

    if not _resolve_cpp_binary():
        pytest.skip("cpp_receiver/build/decode_cli is not built")

    # Keep CI fast by sampling a single deterministic vector
    sample = pairs[:1]
    for vector_path, metadata_path in sample:
        metadata = json.loads(Path(metadata_path).read_text())
        gr_result = run_gnu_radio_decoder(vector_path, metadata)
        assert gr_result["status"] == "success", gr_result.get("stderr")

        expected_hex = _extract_gr_payload_hex(gr_result)
        assert expected_hex, "GNU Radio output did not include payload hex"

        cpp_result = run_lora_lite_cpp(vector_path, metadata)
        assert cpp_result["status"] == "success", cpp_result.get("stderr")
        assert cpp_result.get("payload_hex"), "C++ decoder did not emit payload_hex"

        actual_hex = cpp_result["payload_hex"].strip().lower()
        assert actual_hex == expected_hex, (
            f"Payload mismatch for {vector_path.name}:\n"
            f"  expected: {expected_hex}\n"
            f"  actual:   {actual_hex}\n"
            f"  stdout:   {cpp_result.get('stdout','')}"
        )


# ---------------------------------------------------------------------------
# Optional CLI entry-point for manual parity sweeps.
# ---------------------------------------------------------------------------

def analyze_compatibility(results: List[Dict]) -> Dict:
    total = len(results)
    gr_success = sum(1 for r in results if r["gnu_radio"].get("status") == "success")
    cpp_success = sum(1 for r in results if r["lora_lite_cpp"].get("status") == "success")

    payload_matches = 0
    for entry in results:
        expected = _extract_gr_payload_hex(entry["gnu_radio"])
        cpp_hex = entry["lora_lite_cpp"].get("payload_hex")
        if expected and cpp_hex and entry["lora_lite_cpp"].get("status") == "success":
            if cpp_hex.strip().lower() == expected:
                payload_matches += 1

    return {
        "total_vectors": total,
        "gnu_radio_success_rate": (gr_success / total * 100) if total else 0.0,
        "cpp_success_rate": (cpp_success / total * 100) if total else 0.0,
        "payload_match_rate": (payload_matches / total * 100) if total else 0.0,
        "matches": payload_matches,
    }


def _run_single_vector(vector_path: Path, metadata_path: Path) -> Dict:
    metadata = json.loads(metadata_path.read_text())
    return {
        "vector": str(vector_path),
        "metadata": metadata,
        "gnu_radio": run_gnu_radio_decoder(vector_path, metadata),
        "lora_lite_cpp": run_lora_lite_cpp(vector_path, metadata),
    }


def main() -> None:  # pragma: no cover - manual utility
    import argparse
    import time

    parser = argparse.ArgumentParser(description="GNU Radio vs. C++ receiver comparison")
    parser.add_argument("--vectors-dir", type=Path, default=Path("golden_vectors_demo_batch"))
    parser.add_argument("--limit", type=int, default=None)
    parser.add_argument("--output", type=Path, default=Path("gnu_radio_compat_results.json"))
    args = parser.parse_args()

    if not _have_gnuradio_script():
        raise SystemExit("GNU Radio offline decoder script not available")
    if not _resolve_cpp_binary():
        raise SystemExit("decode_cli binary not found – build cpp_receiver first")

    pairs = _collect_vector_pairs(args.vectors_dir)
    if not pairs:
        raise SystemExit(f"No vectors found in {args.vectors_dir}")
    if args.limit:
        pairs = pairs[: args.limit]

    print("🚀 GNU Radio vs. C++ Receiver")
    print("=" * 48)
    all_results: List[Dict] = []
    for idx, (vec, meta) in enumerate(pairs, start=1):
        print(f"[{idx}/{len(pairs)}] {vec.name}")
        all_results.append(_run_single_vector(vec, meta))

    analysis = analyze_compatibility(all_results)
    print("\n📈 SUMMARY")
    print("=" * 48)
    print(f"Total vectors      : {analysis['total_vectors']}")
    print(f"GNU Radio success  : {analysis['gnu_radio_success_rate']:.1f}%")
    print(f"C++ success        : {analysis['cpp_success_rate']:.1f}%")
    print(f"Payload match rate : {analysis['payload_match_rate']:.1f}%")

    args.output.write_text(json.dumps({
        "analysis": analysis,
        "results": all_results,
        "timestamp": time.time(),
    }, indent=2))
    print(f"\n💾 Results saved to {args.output}")


if __name__ == "__main__":  # pragma: no cover - script entry point
    main()
