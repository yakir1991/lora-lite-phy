#!/usr/bin/env python3
# This file provides the 'compare streaming compat' functionality for the LoRa Lite PHY toolkit.
"""Compare GNU Radio vs. C++ streaming receiver (implicit/embedded profile aware).

This tool runs the GNU Radio offline decoder and the in-tree C++ receiver in
streaming mode (decode_cli --streaming) on a set of cf32 vectors, then compares
the decoded payload hex strings. It reads per-vector metadata from the sibling
.json sidecar to properly configure implicit header mode (embedded profile),
payload length, coding rate, CRC presence, LDRO, and rates.

Usage examples:
  python -m tools.compare_streaming_compat --vectors golden_vectors_demo_batch --limit 1
  python -m tools.compare_streaming_compat --vectors golden_vectors_demo_batch

Outputs a brief summary and stores a JSON report under results/ by default.
"""

# Imports specific objects with 'from __future__ import annotations'.
from __future__ import annotations

# Imports the module(s) argparse.
import argparse
# Imports the module(s) json.
import json
# Imports the module(s) subprocess.
import subprocess
# Imports the module(s) sys.
import sys
# Imports specific objects with 'from dataclasses import dataclass'.
from dataclasses import dataclass
# Imports specific objects with 'from pathlib import Path'.
from pathlib import Path
# Imports specific objects with 'from typing import Dict, List, Optional, Tuple'.
from typing import Dict, List, Optional, Tuple


# Executes the statement `REPO_ROOT = Path(__file__).resolve().parents[1]`.
REPO_ROOT = Path(__file__).resolve().parents[1]
# Executes the statement `GR_SCRIPT = REPO_ROOT / "external/gr_lora_sdr/scripts/decode_offline_recording.py"`.
GR_SCRIPT = REPO_ROOT / "external/gr_lora_sdr/scripts/decode_offline_recording.py"
# Executes the statement `CPP_CANDIDATES = [`.
CPP_CANDIDATES = [
    # Executes the statement `REPO_ROOT / "cpp_receiver/build/decode_cli",`.
    REPO_ROOT / "cpp_receiver/build/decode_cli",
    # Executes the statement `REPO_ROOT / "cpp_receiver/build/Release/decode_cli",`.
    REPO_ROOT / "cpp_receiver/build/Release/decode_cli",
    # Executes the statement `REPO_ROOT / "cpp_receiver/build/Debug/decode_cli",`.
    REPO_ROOT / "cpp_receiver/build/Debug/decode_cli",
    # Executes the statement `REPO_ROOT / "cpp_receiver/build/decode_cli.exe",  # Windows`.
    REPO_ROOT / "cpp_receiver/build/decode_cli.exe",  # Windows
# Closes the previously opened list indexing or literal.
]


# Defines the function have_gnu_radio.
def have_gnu_radio() -> bool:
    # Returns the computed value to the caller.
    return GR_SCRIPT.exists()


# Defines the function resolve_cpp_binary.
def resolve_cpp_binary() -> Optional[Path]:
    # Starts a loop iterating over a sequence.
    for path in CPP_CANDIDATES:
        # Begins a conditional branch to check a condition.
        if path.exists():
            # Returns the computed value to the caller.
            return path
    # Returns the computed value to the caller.
    return None


# Defines the function collect_vector_pairs.
def collect_vector_pairs(vectors_dir: Path) -> List[Tuple[Path, Path]]:
    # Executes the statement `pairs: List[Tuple[Path, Path]] = []`.
    pairs: List[Tuple[Path, Path]] = []
    # Begins a conditional branch to check a condition.
    if not vectors_dir.exists():
        # Returns the computed value to the caller.
        return pairs
    # Starts a loop iterating over a sequence.
    for cf32 in sorted(vectors_dir.glob("*.cf32")):
        # Executes the statement `js = cf32.with_suffix(".json")`.
        js = cf32.with_suffix(".json")
        # Begins a conditional branch to check a condition.
        if js.exists():
            # Executes the statement `pairs.append((cf32, js))`.
            pairs.append((cf32, js))
    # Returns the computed value to the caller.
    return pairs


# Defines the function run_gnu_radio.
def run_gnu_radio(vector_path: Path, meta: Dict) -> Dict:
    # Executes the statement `cmd = [`.
    cmd = [
        # Executes the statement `sys.executable,`.
        sys.executable,
        # Executes the statement `str(GR_SCRIPT),`.
        str(GR_SCRIPT),
        # Executes the statement `str(vector_path),`.
        str(vector_path),
        # Executes the statement `"--sf", str(meta["sf"]),`.
        "--sf", str(meta["sf"]),
        # Executes the statement `"--bw", str(meta["bw"]),`.
        "--bw", str(meta["bw"]),
        # Executes the statement `"--samp-rate", str(meta.get("samp_rate") or meta.get("sample_rate")),`.
        "--samp-rate", str(meta.get("samp_rate") or meta.get("sample_rate")),
        # Executes the statement `"--cr", str(meta["cr"]),`.
        "--cr", str(meta["cr"]),
        # Executes the statement `"--ldro-mode", str(meta.get("ldro_mode", 0)),`.
        "--ldro-mode", str(meta.get("ldro_mode", 0)),
        # Executes the statement `"--format", "cf32",`.
        "--format", "cf32",
    # Closes the previously opened list indexing or literal.
    ]
    # Executes the statement `cmd.append("--has-crc" if meta.get("crc", True) else "--no-crc")`.
    cmd.append("--has-crc" if meta.get("crc", True) else "--no-crc")
    # Executes the statement `cmd.append("--impl-header" if meta.get("impl_header") or meta.get("implicit_header") else "--explicit-header")`.
    cmd.append("--impl-header" if meta.get("impl_header") or meta.get("implicit_header") else "--explicit-header")

    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `res = subprocess.run(cmd, capture_output=True, text=True, timeout=90, errors="replace")`.
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=90, errors="replace")
    # Handles a specific exception from the try block.
    except subprocess.TimeoutExpired:
        # Returns the computed value to the caller.
        return {"status": "timeout", "stdout": "", "stderr": ""}

    # Begins a conditional branch to check a condition.
    if res.returncode != 0:
        # Returns the computed value to the caller.
        return {"status": "failed", "stdout": res.stdout, "stderr": res.stderr, "frames": []}

    # Executes the statement `frames: List[Dict[str, str]] = []`.
    frames: List[Dict[str, str]] = []
    # Starts a loop iterating over a sequence.
    for line in res.stdout.strip().splitlines():
        # Executes the statement `s = line.strip()`.
        s = line.strip()
        # Begins a conditional branch to check a condition.
        if s.startswith("Frame") and ":" in s:
            # Executes the statement `frames.append({"info": s.split(":", 1)[1].strip()})`.
            frames.append({"info": s.split(":", 1)[1].strip()})
        # Handles an additional condition in the branching logic.
        elif s.startswith("Hex:") and frames:
            # Executes the statement `frames[-1]["hex"] = s.replace("Hex:", "").strip()`.
            frames[-1]["hex"] = s.replace("Hex:", "").strip()
        # Handles an additional condition in the branching logic.
        elif s.startswith("Text:") and frames:
            # Executes the statement `frames[-1]["text"] = s.replace("Text:", "").strip()`.
            frames[-1]["text"] = s.replace("Text:", "").strip()
    # Returns the computed value to the caller.
    return {"status": "success", "stdout": res.stdout, "stderr": res.stderr, "frames": frames}


# Defines the function extract_gr_payload_hex.
def extract_gr_payload_hex(result: Dict) -> Optional[str]:
    # Begins a conditional branch to check a condition.
    if not result:
        # Returns the computed value to the caller.
        return None
    # Starts a loop iterating over a sequence.
    for fr in result.get("frames", []):
        # Executes the statement `hx = fr.get("hex")`.
        hx = fr.get("hex")
        # Begins a conditional branch to check a condition.
        if hx:
            # Returns the computed value to the caller.
            return hx.replace(" ", "").lower()
    # Returns the computed value to the caller.
    return None


# Defines the function run_cpp_streaming.
def run_cpp_streaming(vector_path: Path, meta: Dict, opts: argparse.Namespace) -> Dict:
    # Executes the statement `binary = resolve_cpp_binary()`.
    binary = resolve_cpp_binary()
    # Begins a conditional branch to check a condition.
    if not binary:
        # Returns the computed value to the caller.
        return {"status": "skipped", "error": "decode_cli binary not found"}

    # Executes the statement `fs = int(meta.get("samp_rate") or meta.get("sample_rate"))`.
    fs = int(meta.get("samp_rate") or meta.get("sample_rate"))
    # Executes the statement `ldro_mode = int(meta.get("ldro_mode", 0))`.
    ldro_mode = int(meta.get("ldro_mode", 0))
    # Executes the statement `implicit = bool(meta.get("impl_header") or meta.get("implicit_header"))`.
    implicit = bool(meta.get("impl_header") or meta.get("implicit_header"))
    # Executes the statement `has_crc = bool(meta.get("crc", True))`.
    has_crc = bool(meta.get("crc", True))

    # Executes the statement `cmd = [`.
    cmd = [
        # Executes the statement `str(binary),`.
        str(binary),
        # Executes the statement `"--sf", str(meta["sf"]),`.
        "--sf", str(meta["sf"]),
        # Executes the statement `"--bw", str(meta["bw"]),`.
        "--bw", str(meta["bw"]),
        # Executes the statement `"--fs", str(fs),`.
        "--fs", str(fs),
        # Executes the statement `"--ldro", "1" if ldro_mode else "0",`.
        "--ldro", "1" if ldro_mode else "0",
        # Executes the statement `"--streaming",`.
        "--streaming",
        # Executes the statement `str(vector_path),`.
        str(vector_path),
    # Closes the previously opened list indexing or literal.
    ]

    # Implicit header (embedded profile) requires extra parameters
    # Begins a conditional branch to check a condition.
    if implicit:
        # Executes the statement `payload_len = int(meta.get("payload_len") or meta.get("payload_length") or 0)`.
        payload_len = int(meta.get("payload_len") or meta.get("payload_length") or 0)
        # Executes the statement `cr = int(meta.get("cr", 1))`.
        cr = int(meta.get("cr", 1))
        # Executes the statement `cmd.extend(["--implicit-header", "--payload-len", str(payload_len), "--cr", str(cr)])`.
        cmd.extend(["--implicit-header", "--payload-len", str(payload_len), "--cr", str(cr)])
        # Executes the statement `cmd.append("--has-crc" if has_crc else "--no-crc")`.
        cmd.append("--has-crc" if has_crc else "--no-crc")

    # Optional sync word
    # Begins a conditional branch to check a condition.
    if "sync_word" in meta:
        # Executes the statement `cmd.extend(["--sync-word", str(meta["sync_word"])])`.
        cmd.extend(["--sync-word", str(meta["sync_word"])])

    # Optional diagnostics
    # Begins a conditional branch to check a condition.
    if getattr(opts, "hdr_cfo_sweep", False):
        # Executes the statement `cmd.append("--hdr-cfo-sweep")`.
        cmd.append("--hdr-cfo-sweep")
        # Begins a conditional branch to check a condition.
        if getattr(opts, "hdr_cfo_range", None) is not None:
            # Executes the statement `cmd.extend(["--hdr-cfo-range", str(opts.hdr_cfo_range)])`.
            cmd.extend(["--hdr-cfo-range", str(opts.hdr_cfo_range)])
        # Begins a conditional branch to check a condition.
        if getattr(opts, "hdr_cfo_step", None) is not None:
            # Executes the statement `cmd.extend(["--hdr-cfo-step", str(opts.hdr_cfo_step)])`.
            cmd.extend(["--hdr-cfo-step", str(opts.hdr_cfo_step)])
        # Executes the statement `dump_dir = getattr(opts, "dump_header_iq_dir", None)`.
        dump_dir = getattr(opts, "dump_header_iq_dir", None)
    # Begins a conditional branch to check a condition.
    if dump_dir:
        # Create a per-vector filename inside the directory
        # Executes the statement `out_name = vector_path.stem + "_header.cf32"`.
        out_name = vector_path.stem + "_header.cf32"
        # Executes the statement `out_path = Path(dump_dir) / out_name`.
        out_path = Path(dump_dir) / out_name
            # Executes the statement `cmd.extend(["--dump-header-iq", str(out_path)])`.
            cmd.extend(["--dump-header-iq", str(out_path)])
            # If caller provided extra slice controls, pass them through
            # Begins a conditional branch to check a condition.
            if getattr(opts, "dump_header_iq_payload_syms", None) is not None:
                # Executes the statement `cmd.extend(["--dump-header-iq-payload-syms", str(opts.dump_header_iq_payload_syms)])`.
                cmd.extend(["--dump-header-iq-payload-syms", str(opts.dump_header_iq_payload_syms)])
            # Begins a conditional branch to check a condition.
            if getattr(opts, "dump_header_iq_always", False):
                # Executes the statement `cmd.append("--dump-header-iq-always")`.
                cmd.append("--dump-header-iq-always")

    # Begins a block that monitors for exceptions.
    try:
        # Executes the statement `res = subprocess.run(cmd, capture_output=True, text=True, timeout=60, errors="replace")`.
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=60, errors="replace")
    # Handles a specific exception from the try block.
    except subprocess.TimeoutExpired:
        # Returns the computed value to the caller.
        return {"status": "timeout", "stdout": "", "stderr": ""}

    # Executes the statement `payload_hex: Optional[str] = None`.
    payload_hex: Optional[str] = None
    # Starts a loop iterating over a sequence.
    for line in res.stdout.splitlines():
        # Begins a conditional branch to check a condition.
        if line.startswith("payload_hex="):
            # Executes the statement `payload_hex = line.split("=", 1)[1].strip()`.
            payload_hex = line.split("=", 1)[1].strip()
            # Exits the nearest enclosing loop early.
            break

    # Executes the statement `status = "success" if res.returncode == 0 else "failed"`.
    status = "success" if res.returncode == 0 else "failed"
    # Returns the computed value to the caller.
    return {"status": status, "stdout": res.stdout, "stderr": res.stderr, "payload_hex": payload_hex, "binary": str(binary)}


# Executes the statement `@dataclass`.
@dataclass
# Declares the class CompareEntry.
class CompareEntry:
    # Executes the statement `vector: str`.
    vector: str
    # Executes the statement `metadata: Dict`.
    metadata: Dict
    # Executes the statement `gnu_radio: Dict`.
    gnu_radio: Dict
    # Executes the statement `cpp_stream: Dict`.
    cpp_stream: Dict


# Defines the function analyze.
def analyze(results: List[CompareEntry]) -> Dict:
    # Executes the statement `total = len(results)`.
    total = len(results)
    # Executes the statement `gr_ok = sum(1 for r in results if r.gnu_radio.get("status") in ("success", "expected") and extract_gr_payload_hex(r.gnu_radio))`.
    gr_ok = sum(1 for r in results if r.gnu_radio.get("status") in ("success", "expected") and extract_gr_payload_hex(r.gnu_radio))
    # Executes the statement `cpp_ok = sum(1 for r in results if r.cpp_stream.get("status") == "success" and r.cpp_stream.get("payload_hex"))`.
    cpp_ok = sum(1 for r in results if r.cpp_stream.get("status") == "success" and r.cpp_stream.get("payload_hex"))
    # Executes the statement `matches = 0`.
    matches = 0
    # Starts a loop iterating over a sequence.
    for r in results:
        # Executes the statement `exp = extract_gr_payload_hex(r.gnu_radio)`.
        exp = extract_gr_payload_hex(r.gnu_radio)
        # Executes the statement `got = r.cpp_stream.get("payload_hex")`.
        got = r.cpp_stream.get("payload_hex")
        # Begins a conditional branch to check a condition.
        if exp and got and r.cpp_stream.get("status") == "success":
            # Begins a conditional branch to check a condition.
            if got.strip().lower() == exp:
                # Executes the statement `matches += 1`.
                matches += 1
    # Returns the computed value to the caller.
    return {
        # Executes the statement `"total": total,`.
        "total": total,
        # Executes the statement `"gnu_radio_success_rate": (gr_ok / total * 100.0) if total else 0.0,`.
        "gnu_radio_success_rate": (gr_ok / total * 100.0) if total else 0.0,
        # Executes the statement `"cpp_success_rate": (cpp_ok / total * 100.0) if total else 0.0,`.
        "cpp_success_rate": (cpp_ok / total * 100.0) if total else 0.0,
        # Executes the statement `"payload_match_rate": (matches / total * 100.0) if total else 0.0,`.
        "payload_match_rate": (matches / total * 100.0) if total else 0.0,
        # Executes the statement `"matches": matches,`.
        "matches": matches,
    # Closes the previously opened dictionary or set literal.
    }


# Defines the function main.
def main() -> None:
    # Executes the statement `parser = argparse.ArgumentParser(description="GNU Radio vs C++ streaming receiver comparison")`.
    parser = argparse.ArgumentParser(description="GNU Radio vs C++ streaming receiver comparison")
    # Configures the argument parser for the CLI.
    parser.add_argument("--vectors", type=Path, default=REPO_ROOT / "golden_vectors_demo_batch")
    # Configures the argument parser for the CLI.
    parser.add_argument("--limit", type=int, default=None)
    # Configures the argument parser for the CLI.
    parser.add_argument("--output", type=Path, default=REPO_ROOT / "results/streaming_compat_results.json")
    # Configures the argument parser for the CLI.
    parser.add_argument("--json-fallback", action="store_true", help="Use payload_hex from JSON sidecar when GNU Radio is unavailable or fails")
    # Configures the argument parser for the CLI.
    parser.add_argument("--hdr-cfo-sweep", action="store_true", help="Enable small CFO sweep during header decode in C++ receiver")
    # Configures the argument parser for the CLI.
    parser.add_argument("--hdr-cfo-range", type=float, default=None, help="CFO sweep half-range in Hz (passed to decode_cli)")
    # Configures the argument parser for the CLI.
    parser.add_argument("--hdr-cfo-step", type=float, default=None, help="CFO sweep step in Hz (passed to decode_cli)")
        # Configures the argument parser for the CLI.
        parser.add_argument("--dump-header-iq-dir", type=Path, default=None, help="Directory to write header IQ slices (cf32) per vector")
        # Configures the argument parser for the CLI.
        parser.add_argument("--dump-header-iq-payload-syms", type=int, default=128, help="Extra payload symbols to include in slice")
        # Configures the argument parser for the CLI.
        parser.add_argument("--dump-header-iq-always", action="store_true", help="Dump slice even if header fails (diagnostics)")
    # Executes the statement `args = parser.parse_args()`.
    args = parser.parse_args()

    # Begins a conditional branch to check a condition.
    if not have_gnu_radio() and not args.json_fallback:
        # Raises an exception to signal an error.
        raise SystemExit("GNU Radio offline decoder script not available: " + str(GR_SCRIPT))
    # Begins a conditional branch to check a condition.
    if not resolve_cpp_binary():
        # Raises an exception to signal an error.
        raise SystemExit("decode_cli not found; build cpp_receiver first")

    # Executes the statement `pairs = collect_vector_pairs(args.vectors)`.
    pairs = collect_vector_pairs(args.vectors)
    # Begins a conditional branch to check a condition.
    if not pairs:
        # Raises an exception to signal an error.
        raise SystemExit(f"No vectors found in {args.vectors}")
    # Begins a conditional branch to check a condition.
    if args.limit:
        # Executes the statement `pairs = pairs[: args.limit]`.
        pairs = pairs[: args.limit]

    # Outputs diagnostic or user-facing text.
    print("Streaming receiver vs GNU Radio")
    # Outputs diagnostic or user-facing text.
    print("=" * 40)
    # Executes the statement `results: List[CompareEntry] = []`.
    results: List[CompareEntry] = []
    # Starts a loop iterating over a sequence.
    for idx, (vec, meta_path) in enumerate(pairs, start=1):
        # Executes the statement `meta = json.loads(meta_path.read_text())`.
        meta = json.loads(meta_path.read_text())
        # Outputs diagnostic or user-facing text.
        print(f"[{idx}/{len(pairs)}] {vec.name} (impl={bool(meta.get('impl_header') or meta.get('implicit_header'))})")
        # Begins a conditional branch to check a condition.
        if have_gnu_radio():
            # Executes the statement `gr = run_gnu_radio(vec, meta)`.
            gr = run_gnu_radio(vec, meta)
        # Provides the fallback branch when previous conditions fail.
        else:
            # Executes the statement `gr = {"status": "expected", "frames": []}`.
            gr = {"status": "expected", "frames": []}
        # If GR failed and json-fallback requested, synthesize expected frame from sidecar.
        # Begins a conditional branch to check a condition.
        if args.json_fallback and (gr.get("status") != "success"):
            # Executes the statement `exp_hex = str(meta.get("payload_hex") or "")`.
            exp_hex = str(meta.get("payload_hex") or "")
            # Begins a conditional branch to check a condition.
            if exp_hex:
                # Executes the statement `gr = {"status": "expected", "frames": [{"hex": exp_hex}]}`.
                gr = {"status": "expected", "frames": [{"hex": exp_hex}]}
        # Executes the statement `cpp = run_cpp_streaming(vec, meta, args)`.
        cpp = run_cpp_streaming(vec, meta, args)
        # Executes the statement `results.append(CompareEntry(str(vec), meta, gr, cpp))`.
        results.append(CompareEntry(str(vec), meta, gr, cpp))

    # Executes the statement `summary = analyze(results)`.
    summary = analyze(results)
    # Outputs diagnostic or user-facing text.
    print("\nSummary")
    # Outputs diagnostic or user-facing text.
    print("=" * 40)
    # Outputs diagnostic or user-facing text.
    print(f"Total vectors      : {summary['total']}")
    # Outputs diagnostic or user-facing text.
    print(f"GNU Radio success  : {summary['gnu_radio_success_rate']:.1f}%")
    # Outputs diagnostic or user-facing text.
    print(f"C++ success (strm) : {summary['cpp_success_rate']:.1f}%")
    # Outputs diagnostic or user-facing text.
    print(f"Payload match rate : {summary['payload_match_rate']:.1f}%")

    # Accesses a parsed command-line argument.
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Accesses a parsed command-line argument.
    args.output.write_text(json.dumps({
        # Executes the statement `"summary": summary,`.
        "summary": summary,
        # Executes the statement `"results": [`.
        "results": [
            # Executes the statement `{`.
            {
                # Executes the statement `"vector": r.vector,`.
                "vector": r.vector,
                # Executes the statement `"metadata": r.metadata,`.
                "metadata": r.metadata,
                # Executes the statement `"gnu_radio": r.gnu_radio,`.
                "gnu_radio": r.gnu_radio,
                # Executes the statement `"cpp_stream": r.cpp_stream,`.
                "cpp_stream": r.cpp_stream,
            # Executes the statement `} for r in results`.
            } for r in results
        # Executes the statement `],`.
        ],
    # Executes the statement `}, indent=2))`.
    }, indent=2))
    # Outputs diagnostic or user-facing text.
    print(f"\nSaved results to {args.output}")


# Begins a conditional branch to check a condition.
if __name__ == "__main__":
    # Executes the statement `main()`.
    main()
