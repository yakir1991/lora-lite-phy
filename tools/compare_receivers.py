#!/usr/bin/env python3
"""
Unified comparison harness for the C++ LoRa receiver and the GNU Radio reference.

This script consumes a vector manifest (see docs/test_vector_corpus.md) or scans
directories for cf32+json pairs, runs the requested backends, and emits a JSON
report containing per-vector results and key metrics.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict, List, Optional

from .harness import (
    CompareOptions,
    CppStreamingBackend,
    GnuRadioBatchBackend,
    VectorEntry,
    load_manifest_or_scan,
)


def _sanitize_hex(value: Optional[str]) -> Optional[str]:
    if not isinstance(value, str):
        return None
    cleaned = value.strip().replace(" ", "")
    if not cleaned or len(cleaned) % 2 != 0:
        return None
    try:
        bytes.fromhex(cleaned)
    except ValueError:
        return None
    return cleaned.lower()


def _bytes_from_hex(value: Optional[str]) -> Optional[bytes]:
    sanitized = _sanitize_hex(value)
    if sanitized is None:
        return None
    return bytes.fromhex(sanitized)


def _extract_payload_bytes(backend: Dict[str, Any]) -> Optional[bytes]:
    candidates: List[str] = []
    direct = backend.get("payload_hex")
    if isinstance(direct, str):
        candidates.append(direct)

    extra = backend.get("extra")
    if isinstance(extra, dict):
        result_json = extra.get("result_json")
        if isinstance(result_json, dict):
            rj_hex = result_json.get("payload_hex")
            if isinstance(rj_hex, str):
                candidates.append(rj_hex)
            frames = result_json.get("frames")
            if isinstance(frames, list):
                for frame in frames:
                    if isinstance(frame, dict):
                        frame_hex = frame.get("payload_hex")
                        if isinstance(frame_hex, str):
                            candidates.append(frame_hex)

    for cand in candidates:
        payload_bytes = _bytes_from_hex(cand)
        if payload_bytes is not None:
            return payload_bytes
    return None


DEFAULT_OUTPUT = Path("results/compare_receivers.json")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare C++ LoRa receiver and GNU Radio reference decoders"
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=None,
        help="Path to vector manifest JSON; falls back to --vectors scan",
    )
    parser.add_argument(
        "--vectors",
        type=Path,
        default=Path("golden_vectors"),
        help="Directory to scan for cf32+json if manifest not provided",
    )
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="Recursively scan --vectors directory when manifest not provided",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=None,
        help="Process at most N vectors (after manifest/scan ordering)",
    )
    parser.add_argument(
        "--backends",
        type=str,
        default="cpp_stream,gr_batch",
        help="Comma-separated list of backends to run (cpp_stream,gr_batch)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help="Destination JSON report path",
    )
    parser.add_argument(
        "--hdr-cfo-sweep",
        action="store_true",
        help="Enable header CFO sweep in C++ streaming backend",
    )
    parser.add_argument(
        "--hdr-cfo-range",
        type=float,
        default=None,
        help="CFO sweep half-range (Hz)",
    )
    parser.add_argument(
        "--hdr-cfo-step",
        type=float,
        default=None,
        help="CFO sweep step (Hz)",
    )
    parser.add_argument(
        "--streaming-chunk",
        type=int,
        default=4096,
        help="Chunk size for streaming decode",
    )
    parser.add_argument(
        "--dump-header-iq-dir",
        type=Path,
        default=None,
        help="Directory where C++ backend should dump header IQ slices",
    )
    parser.add_argument(
        "--dump-header-iq-payload-syms",
        type=int,
        default=128,
        help="Additional payload symbols for IQ dumps",
    )
    parser.add_argument(
        "--dump-header-iq-always",
        action="store_true",
        help="Dump header IQ even when decode fails",
    )
    return parser.parse_args()


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


def load_entries(args: argparse.Namespace) -> List[VectorEntry]:
    manifest = args.manifest.expanduser().resolve() if args.manifest else None
    vectors_dir = args.vectors.expanduser().resolve()
    entries = load_manifest_or_scan(manifest, vectors_dir, recursive=args.recursive)
    if args.limit:
        entries = entries[: args.limit]
    return entries


def build_backend_map(args: argparse.Namespace) -> Dict[str, object]:
    backends: Dict[str, object] = {}
    requested = {name.strip() for name in args.backends.split(",") if name.strip()}
    if "cpp_stream" in requested:
        backends["cpp_stream"] = CppStreamingBackend()
    if "gr_batch" in requested:
        backends["gr_batch"] = GnuRadioBatchBackend()
    unknown = requested - set(backends.keys())
    if unknown:
        raise SystemExit(f"Unknown backends requested: {', '.join(sorted(unknown))}")
    return backends


def main() -> None:
    args = parse_args()
    options = build_options(args)
    entries = load_entries(args)
    backends = build_backend_map(args)

    if not entries:
        raise SystemExit("No vectors to process")
    if not backends:
        raise SystemExit("No backends selected")

    print(f"Loaded {len(entries)} entries; running backends: {', '.join(backends.keys())}")
    args.output = args.output.expanduser().resolve()
    if args.dump_header_iq_dir:
        args.dump_header_iq_dir = args.dump_header_iq_dir.expanduser().resolve()
        args.dump_header_iq_dir.mkdir(parents=True, exist_ok=True)

    results = []
    for idx, entry in enumerate(entries, start=1):
        print(f"[{idx}/{len(entries)}] {entry.path}")
        vector_result = {
            "path": str(entry.path),
            "metadata": entry.metadata,
            "backends": {},
        }
        analysis: Dict[str, Any] = {
            "metadata_payload_hex": None,
            "metadata_payload_len": None,
            "backend_payload_hex": {},
            "backend_matches_metadata": {},
            "backend_success": {},
            "backend_crc_ok": {},
            "backend_byte_similarity": {},
            "payload_match": False,
        }
        if entry.group:
            vector_result["group"] = entry.group
        if entry.source:
            vector_result["source"] = entry.source
        if entry.notes:
            vector_result["notes"] = entry.notes
        if entry.clean_path:
            vector_result["clean_path"] = str(entry.clean_path)
        if entry.air_path:
            vector_result["air_path"] = str(entry.air_path)

        metadata_hex = _sanitize_hex(entry.metadata.get("payload_hex"))
        metadata_bytes = _bytes_from_hex(metadata_hex)
        if metadata_hex is not None:
            analysis["metadata_payload_hex"] = metadata_hex
        if metadata_bytes is not None:
            analysis["metadata_payload_len"] = len(metadata_bytes)

        backend_payload_bytes: Dict[str, Optional[bytes]] = {}
        for name, backend in backends.items():
            decode = backend.decode(entry.path, entry.metadata, options)  # type: ignore[arg-type]
            backend_dict = decode.to_dict()
            vector_result["backends"][name] = backend_dict
            success = backend_dict.get("status") == "success"
            analysis["backend_success"][name] = success

            payload_bytes = _extract_payload_bytes(backend_dict)
            backend_payload_bytes[name] = payload_bytes
            analysis["backend_payload_hex"][name] = payload_bytes.hex() if payload_bytes is not None else None

            crc_ok = None
            extra = backend_dict.get("extra")
            if isinstance(extra, dict):
                result_json = extra.get("result_json")
                if isinstance(result_json, dict) and isinstance(result_json.get("payload_crc_ok"), bool):
                    crc_ok = result_json.get("payload_crc_ok")
            analysis["backend_crc_ok"][name] = crc_ok

            if payload_bytes is not None and metadata_bytes is not None and len(metadata_bytes) > 0:
                equal = sum(1 for a, b in zip(payload_bytes, metadata_bytes) if a == b)
                ratio = equal / len(metadata_bytes)
                analysis["backend_byte_similarity"][name] = {
                    "matched_bytes": equal,
                    "total_bytes": len(metadata_bytes),
                    "ratio": ratio,
                }
            else:
                analysis["backend_byte_similarity"][name] = None

            matches_metadata = (
                success
                and metadata_bytes is not None
                and payload_bytes == metadata_bytes
            )
            analysis["backend_matches_metadata"][name] = matches_metadata

        if metadata_bytes is not None:
            # require that every backend under test matches metadata for a full payload match
            analysis["payload_match"] = all(
                analysis["backend_matches_metadata"].get(name, False)
                for name in backends.keys()
            )
        else:
            available = [payload for payload in backend_payload_bytes.values() if payload is not None]
            if len(available) >= 2:
                first = available[0]
                analysis["payload_match"] = all(payload == first for payload in available[1:])

        vector_result["analysis"] = analysis
        results.append(vector_result)

    summary = {"total": len(results), "backends": {}}
    for name in backends.keys():
        success = sum(
            1 for item in results if item["analysis"]["backend_success"].get(name, False)
        )
        meta_match = sum(
            1 for item in results if item["analysis"]["backend_matches_metadata"].get(name, False)
        )
        crc_values = [
            item["analysis"]["backend_crc_ok"].get(name)
            for item in results
            if item["analysis"]["backend_crc_ok"].get(name) is not None
        ]
        crc_total = len(crc_values)
        crc_success = sum(1 for v in crc_values if v)
        similarity_values = [
            item["analysis"]["backend_byte_similarity"][name]["ratio"]
            for item in results
            if item["analysis"]["backend_byte_similarity"].get(name) and item["analysis"]["backend_byte_similarity"][name].get("ratio") is not None
        ]
        avg_similarity = sum(similarity_values) / len(similarity_values) if similarity_values else 0.0
        summary["backends"][name] = {
            "success": success,
            "success_rate": (success / len(results) * 100.0) if results else 0.0,
            "metadata_matches": meta_match,
            "metadata_match_rate": (meta_match / len(results) * 100.0) if results else 0.0,
            "crc_checks": crc_total,
            "crc_success": crc_success,
            "crc_success_rate": (crc_success / crc_total * 100.0) if crc_total else None,
            "avg_byte_similarity": avg_similarity,
        }

    payload_matches = sum(1 for item in results if item["analysis"].get("payload_match"))
    summary["payload_matches"] = payload_matches
    summary["payload_match_rate"] = (payload_matches / len(results) * 100.0) if results else 0.0

    print("\nSummary")
    print("=" * 40)
    print(f"Total vectors : {summary['total']}")
    for name, data in summary["backends"].items():
        print(f"{name:>12s} success      : {data['success_rate']:.1f}% ({data['success']}/{summary['total']})")
        print(f"{name:>12s} meta-match   : {data['metadata_match_rate']:.1f}% ({data['metadata_matches']}/{summary['total']})")
        crc_rate = data["crc_success_rate"]
        if crc_rate is not None:
            print(f"{name:>12s} CRC success : {crc_rate:.1f}% ({data['crc_success']}/{data['crc_checks']})")
        else:
            print(f"{name:>12s} CRC success : n/a")
        print(f"{name:>12s} Avg byte sim: {data['avg_byte_similarity']:.2%}")
    print(f"Payload match : {summary['payload_match_rate']:.1f}% ({summary['payload_matches']}/{summary['total']})")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"summary": summary, "results": results}, indent=2))
    print(f"Saved report -> {args.output}")


if __name__ == "__main__":
    main()
