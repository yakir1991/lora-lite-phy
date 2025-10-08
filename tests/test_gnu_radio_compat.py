#!/usr/bin/env python3
# This file provides the 'test gnu radio compat' functionality for the LoRa Lite PHY toolkit.
"""GNU Radio vs. C++ receiver compatibility smoke test.

The project now relies on the vendored GNU Radio implementation under
``external/sdr_lora`` as the Python reference. This test verifies that the
native C++ receiver (`cpp_receiver`) produces the same payload as the GNU Radio
decoder for a small sample of demo vectors.
"""

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) json.
import json
# Imports the module(s) subprocess.
import subprocess
# Imports the module(s) sys.
import sys
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from typing import Dict, List, Optional, Tuple'.
from typing import Dict, List, Optional, Tuple

# Imports the module(s) pytest.
import pytest


# Executes the statement `GR_SCRIPT = Path("external/gr_lora_sdr/scripts/decode_offline_recording.py")`.
GR_SCRIPT = Path("external/gr_lora_sdr/scripts/decode_offline_recording.py")
# Executes the statement `CPP_BINARY_CANDIDATES = (`.
CPP_BINARY_CANDIDATES = (
    # Executes the statement `Path("cpp_receiver/build/decode_cli"),`.
    Path("cpp_receiver/build/decode_cli"),
    # Executes the statement `Path("cpp_receiver/build/Release/decode_cli"),`.
    Path("cpp_receiver/build/Release/decode_cli"),
    # Executes the statement `Path("cpp_receiver/build/Debug/decode_cli"),`.
    Path("cpp_receiver/build/Debug/decode_cli"),
    # Executes the statement `Path("cpp_receiver/build/decode_cli.exe"),  # Windows`.
    Path("cpp_receiver/build/decode_cli.exe"),  # Windows
# Closes the previously opened parenthesis grouping.
)


# Defines the function _have_gnuradio_script.
def _have_gnuradio_script() -> bool:
    # Returns the computed value to the caller.
    return GR_SCRIPT.exists()


# Defines the function _resolve_cpp_binary.
def _resolve_cpp_binary() -> Optional[Path]:
    # Starts a loop iterating over a sequence.
    for candidate in CPP_BINARY_CANDIDATES:
        # Begins a conditional branch to check a condition.
        if candidate.exists():
            # Returns the computed value to the caller.
            return candidate
    # Returns the computed value to the caller.
    return None


# Defines the function _collect_vector_pairs.
def _collect_vector_pairs(vectors_dir: Path) -> List[Tuple[Path, Path]]:
    # Executes the statement `pairs: List[Tuple[Path, Path]] = []`.
    pairs: List[Tuple[Path, Path]] = []
    # Begins a conditional branch to check a condition.
    if not vectors_dir.exists():
        # Returns the computed value to the caller.
        return pairs
    # Starts a loop iterating over a sequence.
    for cf32_file in sorted(vectors_dir.glob("*.cf32")):
        # Executes the statement `json_file = cf32_file.with_suffix(".json")`.
        json_file = cf32_file.with_suffix(".json")
        # Begins a conditional branch to check a condition.
        if json_file.exists():
            # Executes the statement `pairs.append((cf32_file, json_file))`.
            pairs.append((cf32_file, json_file))
    # Returns the computed value to the caller.
    return pairs


# Defines the function run_gnu_radio_decoder.
def run_gnu_radio_decoder(vector_path: Path, metadata: Dict) -> Dict:
    """Run the GNU Radio decoder (Python reference)."""

    # Executes the statement `cmd = [`.
    cmd = [
        # Executes the statement `sys.executable,`.
        sys.executable,
        # Executes the statement `str(GR_SCRIPT),`.
        str(GR_SCRIPT),
        # Executes the statement `str(vector_path),`.
        str(vector_path),
        # Executes the statement `"--sf", str(metadata["sf"]),`.
        "--sf", str(metadata["sf"]),
        # Executes the statement `"--bw", str(metadata["bw"]),`.
        "--bw", str(metadata["bw"]),
        # Executes the statement `"--samp-rate", str(metadata["samp_rate"]),`.
        "--samp-rate", str(metadata["samp_rate"]),
        # Executes the statement `"--cr", str(metadata["cr"]),`.
        "--cr", str(metadata["cr"]),
        # Executes the statement `"--ldro-mode", str(metadata["ldro_mode"]),`.
        "--ldro-mode", str(metadata["ldro_mode"]),
        # Executes the statement `"--format", "cf32",`.
        "--format", "cf32",
    # Closes the previously opened list indexing or literal.
    ]
    # Executes the statement `cmd.append("--has-crc" if metadata.get("crc", True) else "--no-crc")`.
    cmd.append("--has-crc" if metadata.get("crc", True) else "--no-crc")
    # Executes the statement `cmd.append("--impl-header" if metadata.get("impl_header") else "--explicit-header")`.
    cmd.append("--impl-header" if metadata.get("impl_header") else "--explicit-header")

    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `result = subprocess.run(`.
        result = subprocess.run(
            # Executes the statement `cmd,`.
            cmd,
            # Executes the statement `capture_output=True,`.
            capture_output=True,
            # Executes the statement `text=True,`.
            text=True,
            # Executes the statement `timeout=60,`.
            timeout=60,
            # Executes the statement `errors="replace",`.
            errors="replace",
        # Closes the previously opened parenthesis grouping.
        )
    # Handles a specific exception from the try block.
    except subprocess.TimeoutExpired:
        # Returns the computed value to the caller.
        return {"status": "timeout", "stdout": "", "stderr": ""}

    # Begins a conditional branch to check a condition.
    if result.returncode != 0:
        # Returns the computed value to the caller.
        return {
            # Executes the statement `"status": "failed",`.
            "status": "failed",
            # Executes the statement `"stdout": result.stdout,`.
            "stdout": result.stdout,
            # Executes the statement `"stderr": result.stderr,`.
            "stderr": result.stderr,
            # Executes the statement `"frames": [],`.
            "frames": [],
        # Closes the previously opened dictionary or set literal.
        }

    # Executes the statement `frames: List[Dict[str, str]] = []`.
    frames: List[Dict[str, str]] = []
    # Starts a loop iterating over a sequence.
    for line in result.stdout.strip().splitlines():
        # Executes the statement `line = line.strip()`.
        line = line.strip()
        # Begins a conditional branch to check a condition.
        if line.startswith("Frame") and ":" in line:
            # Executes the statement `frames.append({"info": line.split(":", 1)[1].strip()})`.
            frames.append({"info": line.split(":", 1)[1].strip()})
        # Handles an additional condition in the branching logic.
        elif line.startswith("Hex:") and frames:
            # Executes the statement `frames[-1]["hex"] = line.replace("Hex:", "").strip()`.
            frames[-1]["hex"] = line.replace("Hex:", "").strip()
        # Handles an additional condition in the branching logic.
        elif line.startswith("Text:") and frames:
            # Executes the statement `frames[-1]["text"] = line.replace("Text:", "").strip()`.
            frames[-1]["text"] = line.replace("Text:", "").strip()

    # Returns the computed value to the caller.
    return {
        # Executes the statement `"status": "success",`.
        "status": "success",
        # Executes the statement `"stdout": result.stdout,`.
        "stdout": result.stdout,
        # Executes the statement `"stderr": result.stderr,`.
        "stderr": result.stderr,
        # Executes the statement `"frames": frames,`.
        "frames": frames,
    # Closes the previously opened dictionary or set literal.
    }


# Defines the function run_lora_lite_cpp.
def run_lora_lite_cpp(vector_path: Path, metadata: Dict) -> Dict:
    """Run the native C++ receiver (decode_cli)."""

    # Executes the statement `binary = _resolve_cpp_binary()`.
    binary = _resolve_cpp_binary()
    # Begins a conditional branch to check a condition.
    if not binary:
        # Returns the computed value to the caller.
        return {"status": "skipped", "error": "decode_cli binary not found", "payload_hex": None}

    # Executes the statement `cmd = [`.
    cmd = [
        # Executes the statement `str(binary),`.
        str(binary),
        # Executes the statement `"--sf", str(metadata["sf"]),`.
        "--sf", str(metadata["sf"]),
        # Executes the statement `"--bw", str(metadata["bw"]),`.
        "--bw", str(metadata["bw"]),
        # Executes the statement `"--fs", str(metadata["samp_rate"]),`.
        "--fs", str(metadata["samp_rate"]),
        # Executes the statement `"--ldro", "1" if metadata.get("ldro_mode") else "0",`.
        "--ldro", "1" if metadata.get("ldro_mode") else "0",
        # Executes the statement `str(vector_path),`.
        str(vector_path),
    # Closes the previously opened list indexing or literal.
    ]

    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `result = subprocess.run(`.
        result = subprocess.run(
            # Executes the statement `cmd,`.
            cmd,
            # Executes the statement `capture_output=True,`.
            capture_output=True,
            # Executes the statement `text=True,`.
            text=True,
            # Executes the statement `timeout=60,`.
            timeout=60,
            # Executes the statement `errors="replace",`.
            errors="replace",
        # Closes the previously opened parenthesis grouping.
        )
    # Handles a specific exception from the try block.
    except subprocess.TimeoutExpired:
        # Returns the computed value to the caller.
        return {"status": "timeout", "payload_hex": None, "stdout": "", "stderr": ""}

    # Executes the statement `payload_hex: Optional[str] = None`.
    payload_hex: Optional[str] = None
    # Starts a loop iterating over a sequence.
    for line in result.stdout.splitlines():
        # Begins a conditional branch to check a condition.
        if line.startswith("payload_hex="):
            # Executes the statement `payload_hex = line.split("=", 1)[1].strip()`.
            payload_hex = line.split("=", 1)[1].strip()
            # Exits the nearest enclosing loop early.
            break

    # Executes the statement `status = "success" if result.returncode == 0 else "failed"`.
    status = "success" if result.returncode == 0 else "failed"
    # Returns the computed value to the caller.
    return {
        # Executes the statement `"status": status,`.
        "status": status,
        # Executes the statement `"stdout": result.stdout,`.
        "stdout": result.stdout,
        # Executes the statement `"stderr": result.stderr,`.
        "stderr": result.stderr,
        # Executes the statement `"payload_hex": payload_hex,`.
        "payload_hex": payload_hex,
        # Executes the statement `"binary": str(binary),`.
        "binary": str(binary),
    # Closes the previously opened dictionary or set literal.
    }


# Defines the function _extract_gr_payload_hex.
def _extract_gr_payload_hex(result: Dict) -> Optional[str]:
    # Begins a conditional branch to check a condition.
    if not result or result.get("status") != "success":
        # Returns the computed value to the caller.
        return None
    # Starts a loop iterating over a sequence.
    for frame in result.get("frames", []):
        # Executes the statement `hex_value = frame.get("hex")`.
        hex_value = frame.get("hex")
        # Begins a conditional branch to check a condition.
        if hex_value:
            # Returns the computed value to the caller.
            return hex_value.replace(" ", "").lower()
    # Returns the computed value to the caller.
    return None


# Executes the statement `@pytest.mark.skipif(not _have_gnuradio_script(), reason="GNU Radio offline decoder not available")`.
@pytest.mark.skipif(not _have_gnuradio_script(), reason="GNU Radio offline decoder not available")
# Defines the function test_cpp_receiver_matches_gnu_radio.
def test_cpp_receiver_matches_gnu_radio():
    # Executes the statement `vectors_dir = Path("golden_vectors_demo_batch")`.
    vectors_dir = Path("golden_vectors_demo_batch")
    # Executes the statement `pairs = _collect_vector_pairs(vectors_dir)`.
    pairs = _collect_vector_pairs(vectors_dir)
    # Begins a conditional branch to check a condition.
    if not pairs:
        # Executes the statement `pytest.skip("no vectors found in golden_vectors_demo_batch")`.
        pytest.skip("no vectors found in golden_vectors_demo_batch")

    # Begins a conditional branch to check a condition.
    if not _resolve_cpp_binary():
        # Executes the statement `pytest.skip("cpp_receiver/build/decode_cli is not built")`.
        pytest.skip("cpp_receiver/build/decode_cli is not built")

    # Keep CI fast by sampling a single deterministic vector
    # Executes the statement `sample = pairs[:1]`.
    sample = pairs[:1]
    # Starts a loop iterating over a sequence.
    for vector_path, metadata_path in sample:
        # Executes the statement `metadata = json.loads(Path(metadata_path).read_text())`.
        metadata = json.loads(Path(metadata_path).read_text())
        # Executes the statement `gr_result = run_gnu_radio_decoder(vector_path, metadata)`.
        gr_result = run_gnu_radio_decoder(vector_path, metadata)
        # Asserts that a condition holds true during execution.
        assert gr_result["status"] == "success", gr_result.get("stderr")

        # Executes the statement `expected_hex = _extract_gr_payload_hex(gr_result)`.
        expected_hex = _extract_gr_payload_hex(gr_result)
        # Asserts that a condition holds true during execution.
        assert expected_hex, "GNU Radio output did not include payload hex"

        # Executes the statement `cpp_result = run_lora_lite_cpp(vector_path, metadata)`.
        cpp_result = run_lora_lite_cpp(vector_path, metadata)
        # Asserts that a condition holds true during execution.
        assert cpp_result["status"] == "success", cpp_result.get("stderr")
        # Asserts that a condition holds true during execution.
        assert cpp_result.get("payload_hex"), "C++ decoder did not emit payload_hex"

        # Executes the statement `actual_hex = cpp_result["payload_hex"].strip().lower()`.
        actual_hex = cpp_result["payload_hex"].strip().lower()
        # Asserts that a condition holds true during execution.
        assert actual_hex == expected_hex, (
            # Executes the statement `f"Payload mismatch for {vector_path.name}:\n"`.
            f"Payload mismatch for {vector_path.name}:\n"
            # Executes the statement `f"  expected: {expected_hex}\n"`.
            f"  expected: {expected_hex}\n"
            # Executes the statement `f"  actual:   {actual_hex}\n"`.
            f"  actual:   {actual_hex}\n"
            # Executes the statement `f"  stdout:   {cpp_result.get('stdout','')}"`.
            f"  stdout:   {cpp_result.get('stdout','')}"
        # Closes the previously opened parenthesis grouping.
        )


# ---------------------------------------------------------------------------
# Optional CLI entry-point for manual parity sweeps.
# ---------------------------------------------------------------------------

# Defines the function analyze_compatibility.
def analyze_compatibility(results: List[Dict]) -> Dict:
    # Executes the statement `total = len(results)`.
    total = len(results)
    # Executes the statement `gr_success = sum(1 for r in results if r["gnu_radio"].get("status") == "success")`.
    gr_success = sum(1 for r in results if r["gnu_radio"].get("status") == "success")
    # Executes the statement `cpp_success = sum(1 for r in results if r["lora_lite_cpp"].get("status") == "success")`.
    cpp_success = sum(1 for r in results if r["lora_lite_cpp"].get("status") == "success")

    # Executes the statement `payload_matches = 0`.
    payload_matches = 0
    # Starts a loop iterating over a sequence.
    for entry in results:
        # Executes the statement `expected = _extract_gr_payload_hex(entry["gnu_radio"])`.
        expected = _extract_gr_payload_hex(entry["gnu_radio"])
        # Executes the statement `cpp_hex = entry["lora_lite_cpp"].get("payload_hex")`.
        cpp_hex = entry["lora_lite_cpp"].get("payload_hex")
        # Begins a conditional branch to check a condition.
        if expected and cpp_hex and entry["lora_lite_cpp"].get("status") == "success":
            # Begins a conditional branch to check a condition.
            if cpp_hex.strip().lower() == expected:
                # Executes the statement `payload_matches += 1`.
                payload_matches += 1

    # Returns the computed value to the caller.
    return {
        # Executes the statement `"total_vectors": total,`.
        "total_vectors": total,
        # Executes the statement `"gnu_radio_success_rate": (gr_success / total * 100) if total else 0.0,`.
        "gnu_radio_success_rate": (gr_success / total * 100) if total else 0.0,
        # Executes the statement `"cpp_success_rate": (cpp_success / total * 100) if total else 0.0,`.
        "cpp_success_rate": (cpp_success / total * 100) if total else 0.0,
        # Executes the statement `"payload_match_rate": (payload_matches / total * 100) if total else 0.0,`.
        "payload_match_rate": (payload_matches / total * 100) if total else 0.0,
        # Executes the statement `"matches": payload_matches,`.
        "matches": payload_matches,
    # Closes the previously opened dictionary or set literal.
    }


# Defines the function _run_single_vector.
def _run_single_vector(vector_path: Path, metadata_path: Path) -> Dict:
    # Executes the statement `metadata = json.loads(metadata_path.read_text())`.
    metadata = json.loads(metadata_path.read_text())
    # Returns the computed value to the caller.
    return {
        # Executes the statement `"vector": str(vector_path),`.
        "vector": str(vector_path),
        # Executes the statement `"metadata": metadata,`.
        "metadata": metadata,
        # Executes the statement `"gnu_radio": run_gnu_radio_decoder(vector_path, metadata),`.
        "gnu_radio": run_gnu_radio_decoder(vector_path, metadata),
        # Executes the statement `"lora_lite_cpp": run_lora_lite_cpp(vector_path, metadata),`.
        "lora_lite_cpp": run_lora_lite_cpp(vector_path, metadata),
    # Closes the previously opened dictionary or set literal.
    }


# Defines the function main.
def main() -> None:  # pragma: no cover - manual utility
    # Imports the module(s) argparse.
    import argparse
    # Imports the module(s) time.
    import time

    # Executes the statement `parser = argparse.ArgumentParser(description="GNU Radio vs. C++ receiver comparison")`.
    parser = argparse.ArgumentParser(description="GNU Radio vs. C++ receiver comparison")
    # Configures the argument parser for the CLI.
    parser.add_argument("--vectors-dir", type=Path, default=Path("golden_vectors_demo_batch"))
    # Configures the argument parser for the CLI.
    parser.add_argument("--limit", type=int, default=None)
    # Configures the argument parser for the CLI.
    parser.add_argument("--output", type=Path, default=Path("gnu_radio_compat_results.json"))
    # Executes the statement `args = parser.parse_args()`.
    args = parser.parse_args()

    # Begins a conditional branch to check a condition.
    if not _have_gnuradio_script():
        # Raises an exception to signal an error.
        raise SystemExit("GNU Radio offline decoder script not available")
    # Begins a conditional branch to check a condition.
    if not _resolve_cpp_binary():
        # Raises an exception to signal an error.
        raise SystemExit("decode_cli binary not found – build cpp_receiver first")

    # Executes the statement `pairs = _collect_vector_pairs(args.vectors_dir)`.
    pairs = _collect_vector_pairs(args.vectors_dir)
    # Begins a conditional branch to check a condition.
    if not pairs:
        # Raises an exception to signal an error.
        raise SystemExit(f"No vectors found in {args.vectors_dir}")
    # Begins a conditional branch to check a condition.
    if args.limit:
        # Executes the statement `pairs = pairs[: args.limit]`.
        pairs = pairs[: args.limit]

    # Outputs diagnostic or user-facing text.
    print("🚀 GNU Radio vs. C++ Receiver")
    # Outputs diagnostic or user-facing text.
    print("=" * 48)
    # Executes the statement `all_results: List[Dict] = []`.
    all_results: List[Dict] = []
    # Starts a loop iterating over a sequence.
    for idx, (vec, meta) in enumerate(pairs, start=1):
        # Outputs diagnostic or user-facing text.
        print(f"[{idx}/{len(pairs)}] {vec.name}")
        # Executes the statement `all_results.append(_run_single_vector(vec, meta))`.
        all_results.append(_run_single_vector(vec, meta))

    # Executes the statement `analysis = analyze_compatibility(all_results)`.
    analysis = analyze_compatibility(all_results)
    # Outputs diagnostic or user-facing text.
    print("\n📈 SUMMARY")
    # Outputs diagnostic or user-facing text.
    print("=" * 48)
    # Outputs diagnostic or user-facing text.
    print(f"Total vectors      : {analysis['total_vectors']}")
    # Outputs diagnostic or user-facing text.
    print(f"GNU Radio success  : {analysis['gnu_radio_success_rate']:.1f}%")
    # Outputs diagnostic or user-facing text.
    print(f"C++ success        : {analysis['cpp_success_rate']:.1f}%")
    # Outputs diagnostic or user-facing text.
    print(f"Payload match rate : {analysis['payload_match_rate']:.1f}%")

    # Accesses a parsed command-line argument.
    args.output.write_text(json.dumps({
        # Executes the statement `"analysis": analysis,`.
        "analysis": analysis,
        # Executes the statement `"results": all_results,`.
        "results": all_results,
        # Executes the statement `"timestamp": time.time(),`.
        "timestamp": time.time(),
    # Executes the statement `}, indent=2))`.
    }, indent=2))
    # Outputs diagnostic or user-facing text.
    print(f"\n💾 Results saved to {args.output}")


# Begins a conditional branch to check a condition.
if __name__ == "__main__":  # pragma: no cover - script entry point
    # Executes the statement `main()`.
    main()
