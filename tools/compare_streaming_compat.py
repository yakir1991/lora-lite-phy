#!/usr/bin/env python3
"""
Compare the GNU Radio reference decoder against the in-tree C++ streaming receiver.

This script now delegates subprocess handling to reusable backend classes housed
under ``tools.harness`` so that higher-level automation can share the same
invocation logic.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

from .harness import (
    CompareOptions,
    CppStreamingBackend,
    DecodeResult,
    GnuRadioBatchBackend,
    collect_vector_pairs,
)


@dataclass
class CompareEntry:
    vector: str
    metadata: Dict[str, Any]
    gnu_radio: DecodeResult
    cpp_stream: DecodeResult

    def to_dict(self) -> Dict[str, Any]:
        return {
            "vector": self.vector,
            "metadata": self.metadata,
            "gnu_radio": self.gnu_radio.to_dict(),
            "cpp_stream": self.cpp_stream.to_dict(),
        }


def extract_payload_hex(result: DecodeResult) -> Optional[str]:
    if not result:
        return None
    if result.payload_hex:
        return result.payload_hex.strip().lower()
    metrics = result.extra.get("result_json") if isinstance(result.extra, dict) else None
    if isinstance(metrics, dict):
        raw_hex = metrics.get("payload_hex")
        if isinstance(raw_hex, str):
            raw_hex = raw_hex.strip()
            if raw_hex:
                return raw_hex.lower()
    for frame in result.frames:
        hx = frame.get("hex")
        if hx:
            return str(hx).strip().lower()
    return None


def analyze(results: List[CompareEntry]) -> Dict[str, float]:
    total = len(results)
    if total == 0:
        return {
            "total": 0,
            "gnu_radio_success_rate": 0.0,
            "cpp_success_rate": 0.0,
            "payload_match_rate": 0.0,
            "matches": 0,
        }

    gr_ok = sum(
        1
        for r in results
        if r.gnu_radio.status in ("success", "expected") and extract_payload_hex(r.gnu_radio)
    )
    cpp_ok = sum(
        1
        for r in results
        if r.cpp_stream.status == "success" and extract_payload_hex(r.cpp_stream)
    )

    matches = 0
    for r in results:
        exp = extract_payload_hex(r.gnu_radio)
        got = extract_payload_hex(r.cpp_stream)
        if exp and got and r.cpp_stream.status == "success":
            if got == exp:
                matches += 1

    return {
        "total": total,
        "gnu_radio_success_rate": gr_ok / total * 100.0,
        "cpp_success_rate": cpp_ok / total * 100.0,
        "payload_match_rate": matches / total * 100.0,
        "matches": matches,
    }


def build_options(args: argparse.Namespace) -> CompareOptions:
    return CompareOptions(
        streaming_chunk=args.streaming_chunk,
        hdr_cfo_sweep=args.hdr_cfo_sweep,
        hdr_cfo_range=args.hdr_cfo_range,
        hdr_cfo_step=args.hdr_cfo_step,
        dump_header_iq_dir=args.dump_header_iq_dir,
        dump_header_iq_payload_syms=args.dump_header_iq_payload_syms,
        dump_header_iq_always=args.dump_header_iq_always,
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="GNU Radio vs C++ streaming receiver comparison"
    )
    parser.add_argument("--vectors", type=Path, default=Path("golden_vectors_demo_batch"))
    parser.add_argument("--limit", type=int, default=None)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("results/streaming_compat_results.json"),
    )
    parser.add_argument(
        "--json-fallback",
        action="store_true",
        help="Use payload_hex from JSON sidecar when GNU Radio is unavailable or fails",
    )
    parser.add_argument(
        "--hdr-cfo-sweep",
        action="store_true",
        help="Enable small CFO sweep during header decode in C++ receiver",
    )
    parser.add_argument(
        "--hdr-cfo-range",
        type=float,
        default=None,
        help="CFO sweep half-range in Hz (passed to decode_cli)",
    )
    parser.add_argument(
        "--hdr-cfo-step",
        type=float,
        default=None,
        help="CFO sweep step in Hz (passed to decode_cli)",
    )
    parser.add_argument(
        "--streaming-chunk",
        type=int,
        default=4096,
        help="Chunk size passed to decode_cli --streaming",
    )
    parser.add_argument(
        "--dump-header-iq-dir",
        type=Path,
        default=None,
        help="Directory to write header IQ slices (cf32) per vector",
    )
    parser.add_argument(
        "--dump-header-iq-payload-syms",
        type=int,
        default=128,
        help="Extra payload symbols to include in slice",
    )
    parser.add_argument(
        "--dump-header-iq-always",
        action="store_true",
        help="Dump slice even if header fails (diagnostics)",
    )
    args = parser.parse_args()

    args.vectors = args.vectors.expanduser().resolve()
    args.output = args.output.expanduser().resolve()
    if args.dump_header_iq_dir:
        args.dump_header_iq_dir = args.dump_header_iq_dir.expanduser().resolve()

    options = build_options(args)

    gr_backend = GnuRadioBatchBackend()
    if not gr_backend.available() and not args.json_fallback:
        raise SystemExit(f"GNU Radio offline decoder script not available: {gr_backend.script}")

    try:
        cpp_backend = CppStreamingBackend()
    except FileNotFoundError as exc:
        raise SystemExit(str(exc)) from exc

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
        print(
            f"[{idx}/{len(pairs)}] {vec.name} "
            f"(impl={bool(meta.get('impl_header') or meta.get('implicit_header'))})"
        )

        if gr_backend.available():
            gr_result = gr_backend.decode(vec, meta, options)
        else:
            gr_result = DecodeResult(
                status="skipped",
                stderr=f"Missing GNU Radio script: {gr_backend.script}",
            )

        if args.json_fallback and not extract_payload_hex(gr_result):
            fallback_hex = str(meta.get("payload_hex") or "").strip()
            if fallback_hex:
                updated_extra = dict(gr_result.extra)
                updated_extra["json_fallback"] = True
                gr_result = DecodeResult(
                    status="expected",
                    payload_hex=fallback_hex.lower(),
                    stdout=gr_result.stdout,
                    stderr=gr_result.stderr,
                    command=gr_result.command,
                    duration_s=gr_result.duration_s,
                    frames=gr_result.frames,
                    extra=updated_extra,
                )

        cpp_result = cpp_backend.decode(vec, meta, options)
        results.append(CompareEntry(str(vec), meta, gr_result, cpp_result))

    summary = analyze(results)
    print("\nSummary")
    print("=" * 40)
    print(f"Total vectors      : {summary['total']}")
    print(f"GNU Radio success  : {summary['gnu_radio_success_rate']:.1f}%")
    print(f"C++ success (strm) : {summary['cpp_success_rate']:.1f}%")
    print(f"Payload match rate : {summary['payload_match_rate']:.1f}%")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(
            {
                "summary": summary,
                "results": [entry.to_dict() for entry in results],
            },
            indent=2,
        )
    )
    print(f"\nSaved results to {args.output}")


if __name__ == "__main__":
    main()
