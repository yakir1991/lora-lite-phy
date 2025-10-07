#!/usr/bin/env python3
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

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parents[1]
GR_SCRIPT = REPO_ROOT / "external/gr_lora_sdr/scripts/decode_offline_recording.py"
CPP_CANDIDATES = [
    REPO_ROOT / "cpp_receiver/build/decode_cli",
    REPO_ROOT / "cpp_receiver/build/Release/decode_cli",
    REPO_ROOT / "cpp_receiver/build/Debug/decode_cli",
    REPO_ROOT / "cpp_receiver/build/decode_cli.exe",  # Windows
]


def have_gnu_radio() -> bool:
    return GR_SCRIPT.exists()


def resolve_cpp_binary() -> Optional[Path]:
    for path in CPP_CANDIDATES:
        if path.exists():
            return path
    return None


def collect_vector_pairs(vectors_dir: Path) -> List[Tuple[Path, Path]]:
    pairs: List[Tuple[Path, Path]] = []
    if not vectors_dir.exists():
        return pairs
    for cf32 in sorted(vectors_dir.glob("*.cf32")):
        js = cf32.with_suffix(".json")
        if js.exists():
            pairs.append((cf32, js))
    return pairs


def run_gnu_radio(vector_path: Path, meta: Dict) -> Dict:
    cmd = [
        sys.executable,
        str(GR_SCRIPT),
        str(vector_path),
        "--sf", str(meta["sf"]),
        "--bw", str(meta["bw"]),
        "--samp-rate", str(meta.get("samp_rate") or meta.get("sample_rate")),
        "--cr", str(meta["cr"]),
        "--ldro-mode", str(meta.get("ldro_mode", 0)),
        "--format", "cf32",
    ]
    cmd.append("--has-crc" if meta.get("crc", True) else "--no-crc")
    cmd.append("--impl-header" if meta.get("impl_header") or meta.get("implicit_header") else "--explicit-header")

    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=90, errors="replace")
    except subprocess.TimeoutExpired:
        return {"status": "timeout", "stdout": "", "stderr": ""}

    if res.returncode != 0:
        return {"status": "failed", "stdout": res.stdout, "stderr": res.stderr, "frames": []}

    frames: List[Dict[str, str]] = []
    for line in res.stdout.strip().splitlines():
        s = line.strip()
        if s.startswith("Frame") and ":" in s:
            frames.append({"info": s.split(":", 1)[1].strip()})
        elif s.startswith("Hex:") and frames:
            frames[-1]["hex"] = s.replace("Hex:", "").strip()
        elif s.startswith("Text:") and frames:
            frames[-1]["text"] = s.replace("Text:", "").strip()
    return {"status": "success", "stdout": res.stdout, "stderr": res.stderr, "frames": frames}


def extract_gr_payload_hex(result: Dict) -> Optional[str]:
    if not result:
        return None
    for fr in result.get("frames", []):
        hx = fr.get("hex")
        if hx:
            return hx.replace(" ", "").lower()
    return None


def run_cpp_streaming(vector_path: Path, meta: Dict, opts: argparse.Namespace) -> Dict:
    binary = resolve_cpp_binary()
    if not binary:
        return {"status": "skipped", "error": "decode_cli binary not found"}

    fs = int(meta.get("samp_rate") or meta.get("sample_rate"))
    ldro_mode = int(meta.get("ldro_mode", 0))
    implicit = bool(meta.get("impl_header") or meta.get("implicit_header"))
    has_crc = bool(meta.get("crc", True))

    cmd = [
        str(binary),
        "--sf", str(meta["sf"]),
        "--bw", str(meta["bw"]),
        "--fs", str(fs),
        "--ldro", "1" if ldro_mode else "0",
        "--streaming",
        str(vector_path),
    ]

    # Implicit header (embedded profile) requires extra parameters
    if implicit:
        payload_len = int(meta.get("payload_len") or meta.get("payload_length") or 0)
        cr = int(meta.get("cr", 1))
        cmd.extend(["--implicit-header", "--payload-len", str(payload_len), "--cr", str(cr)])
        cmd.append("--has-crc" if has_crc else "--no-crc")

    # Optional sync word
    if "sync_word" in meta:
        cmd.extend(["--sync-word", str(meta["sync_word"])])

    # Optional diagnostics
    if getattr(opts, "hdr_cfo_sweep", False):
        cmd.append("--hdr-cfo-sweep")
        if getattr(opts, "hdr_cfo_range", None) is not None:
            cmd.extend(["--hdr-cfo-range", str(opts.hdr_cfo_range)])
        if getattr(opts, "hdr_cfo_step", None) is not None:
            cmd.extend(["--hdr-cfo-step", str(opts.hdr_cfo_step)])
        dump_dir = getattr(opts, "dump_header_iq_dir", None)
    if dump_dir:
        # Create a per-vector filename inside the directory
        out_name = vector_path.stem + "_header.cf32"
        out_path = Path(dump_dir) / out_name
            cmd.extend(["--dump-header-iq", str(out_path)])
            # If caller provided extra slice controls, pass them through
            if getattr(opts, "dump_header_iq_payload_syms", None) is not None:
                cmd.extend(["--dump-header-iq-payload-syms", str(opts.dump_header_iq_payload_syms)])
            if getattr(opts, "dump_header_iq_always", False):
                cmd.append("--dump-header-iq-always")

    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=60, errors="replace")
    except subprocess.TimeoutExpired:
        return {"status": "timeout", "stdout": "", "stderr": ""}

    payload_hex: Optional[str] = None
    for line in res.stdout.splitlines():
        if line.startswith("payload_hex="):
            payload_hex = line.split("=", 1)[1].strip()
            break

    status = "success" if res.returncode == 0 else "failed"
    return {"status": status, "stdout": res.stdout, "stderr": res.stderr, "payload_hex": payload_hex, "binary": str(binary)}


@dataclass
class CompareEntry:
    vector: str
    metadata: Dict
    gnu_radio: Dict
    cpp_stream: Dict


def analyze(results: List[CompareEntry]) -> Dict:
    total = len(results)
    gr_ok = sum(1 for r in results if r.gnu_radio.get("status") in ("success", "expected") and extract_gr_payload_hex(r.gnu_radio))
    cpp_ok = sum(1 for r in results if r.cpp_stream.get("status") == "success" and r.cpp_stream.get("payload_hex"))
    matches = 0
    for r in results:
        exp = extract_gr_payload_hex(r.gnu_radio)
        got = r.cpp_stream.get("payload_hex")
        if exp and got and r.cpp_stream.get("status") == "success":
            if got.strip().lower() == exp:
                matches += 1
    return {
        "total": total,
        "gnu_radio_success_rate": (gr_ok / total * 100.0) if total else 0.0,
        "cpp_success_rate": (cpp_ok / total * 100.0) if total else 0.0,
        "payload_match_rate": (matches / total * 100.0) if total else 0.0,
        "matches": matches,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="GNU Radio vs C++ streaming receiver comparison")
    parser.add_argument("--vectors", type=Path, default=REPO_ROOT / "golden_vectors_demo_batch")
    parser.add_argument("--limit", type=int, default=None)
    parser.add_argument("--output", type=Path, default=REPO_ROOT / "results/streaming_compat_results.json")
    parser.add_argument("--json-fallback", action="store_true", help="Use payload_hex from JSON sidecar when GNU Radio is unavailable or fails")
    parser.add_argument("--hdr-cfo-sweep", action="store_true", help="Enable small CFO sweep during header decode in C++ receiver")
    parser.add_argument("--hdr-cfo-range", type=float, default=None, help="CFO sweep half-range in Hz (passed to decode_cli)")
    parser.add_argument("--hdr-cfo-step", type=float, default=None, help="CFO sweep step in Hz (passed to decode_cli)")
        parser.add_argument("--dump-header-iq-dir", type=Path, default=None, help="Directory to write header IQ slices (cf32) per vector")
        parser.add_argument("--dump-header-iq-payload-syms", type=int, default=128, help="Extra payload symbols to include in slice")
        parser.add_argument("--dump-header-iq-always", action="store_true", help="Dump slice even if header fails (diagnostics)")
    args = parser.parse_args()

    if not have_gnu_radio() and not args.json_fallback:
        raise SystemExit("GNU Radio offline decoder script not available: " + str(GR_SCRIPT))
    if not resolve_cpp_binary():
        raise SystemExit("decode_cli not found; build cpp_receiver first")

    pairs = collect_vector_pairs(args.vectors)
    if not pairs:
        raise SystemExit(f"No vectors found in {args.vectors}")
    if args.limit:
        pairs = pairs[: args.limit]

    print("Streaming receiver vs GNU Radio")
    print("=" * 40)
    results: List[CompareEntry] = []
    for idx, (vec, meta_path) in enumerate(pairs, start=1):
        meta = json.loads(meta_path.read_text())
        print(f"[{idx}/{len(pairs)}] {vec.name} (impl={bool(meta.get('impl_header') or meta.get('implicit_header'))})")
        if have_gnu_radio():
            gr = run_gnu_radio(vec, meta)
        else:
            gr = {"status": "expected", "frames": []}
        # If GR failed and json-fallback requested, synthesize expected frame from sidecar.
        if args.json_fallback and (gr.get("status") != "success"):
            exp_hex = str(meta.get("payload_hex") or "")
            if exp_hex:
                gr = {"status": "expected", "frames": [{"hex": exp_hex}]}
        cpp = run_cpp_streaming(vec, meta, args)
        results.append(CompareEntry(str(vec), meta, gr, cpp))

    summary = analyze(results)
    print("\nSummary")
    print("=" * 40)
    print(f"Total vectors      : {summary['total']}")
    print(f"GNU Radio success  : {summary['gnu_radio_success_rate']:.1f}%")
    print(f"C++ success (strm) : {summary['cpp_success_rate']:.1f}%")
    print(f"Payload match rate : {summary['payload_match_rate']:.1f}%")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({
        "summary": summary,
        "results": [
            {
                "vector": r.vector,
                "metadata": r.metadata,
                "gnu_radio": r.gnu_radio,
                "cpp_stream": r.cpp_stream,
            } for r in results
        ],
    }, indent=2))
    print(f"\nSaved results to {args.output}")


if __name__ == "__main__":
    main()
