#!/usr/bin/env python3

"""Regenerate GNU Radio payload binaries for build/receiver_vs_gnuradio captures.

Why this exists:
- Some existing *_gnuradio_payload.bin artifacts were produced under different semantics
  (e.g., bypassed CRC gating, partial buffers, old tooling), which makes strict
  payload-byte compatibility checks noisy.
- This script re-runs tools/gr_decode_capture.py (GNU Radio) for each capture using
  the per-capture standalone summary metadata (plus minimal inference for missing
  fields), and writes fresh payload binaries into a separate output directory.

This script expects GNU Radio + gr-lora_sdr Python bindings to be importable by the
interpreter used for tools/gr_decode_capture.py. In this repo that is typically:
  /usr/bin/python3

Exit code:
  0 on success
  1 if any case fails to regenerate
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_IN_DIR = REPO_ROOT / "build" / "receiver_vs_gnuradio"
DEFAULT_OUT_DIR = REPO_ROOT / "build" / "receiver_vs_gnuradio_regen_gnu"
DEFAULT_GR_PYTHON = Path("/usr/bin/python3")


@dataclass(frozen=True)
class Case:
    base: str
    capture: Path
    summary: Path


def _load_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text())


def _infer_ldro(sf: int, bw: int) -> bool:
    # LoRa guidance: enable when symbol duration >= 16ms.
    # Ts = 2^SF / BW
    ts = (2.0 ** float(sf)) / float(bw)
    return bool(ts >= 0.016)


def _infer_bool_from_name(base: str, key: str) -> Optional[bool]:
    lowered = base.lower()
    if key == "implicit_header":
        if "implicithdr" in lowered:
            return True
        return None
    if key == "has_crc":
        if "nocrc" in lowered:
            return False
        return None
    return None


def _build_metadata(base: str, summary_json: Dict[str, Any]) -> Dict[str, Any]:
    meta = dict(summary_json.get("metadata", {}))

    # Required core fields.
    for required in ("sf", "bw", "sample_rate", "cr", "payload_len", "preamble_len"):
        if required not in meta:
            raise ValueError(f"summary missing required metadata field {required!r}")

    # Optional booleans: infer if missing.
    if "implicit_header" not in meta:
        inferred = _infer_bool_from_name(base, "implicit_header")
        if inferred is not None:
            meta["implicit_header"] = inferred
        else:
            meta["implicit_header"] = False

    if "has_crc" not in meta:
        inferred = _infer_bool_from_name(base, "has_crc")
        if inferred is not None:
            meta["has_crc"] = inferred
        else:
            meta["has_crc"] = True

    # LDRO: may be absent or encoded as int in some upstream JSONs.
    if "ldro" not in meta:
        meta["ldro"] = _infer_ldro(int(meta["sf"]), int(meta["bw"]))
    else:
        meta["ldro"] = bool(int(meta["ldro"])) if isinstance(meta["ldro"], int) else bool(meta["ldro"])

    # GNU tool supports sync_word list/int; keep default if absent.
    meta.setdefault("sync_word", 0x12)

    return meta


def discover_cases(in_dir: Path) -> List[Case]:
    cases: List[Case] = []
    for summary in sorted(in_dir.glob("*_standalone_summary.json")):
        base = summary.name[: -len("_standalone_summary.json")]
        capture = in_dir / f"{base}.cf32"
        if not capture.exists():
            # Some summaries may point to a work dir; skip here.
            continue
        cases.append(Case(base=base, capture=capture, summary=summary))
    return cases


def run_gr_decode(
    gr_python: Path,
    capture: Path,
    meta: Dict[str, Any],
    meta_path: Path,
    out_payload: Path,
    timeout_s: float,
) -> subprocess.CompletedProcess[bytes]:
    cmd = [
        str(gr_python),
        str(REPO_ROOT / "tools" / "gr_decode_capture.py"),
        "--input",
        str(capture),
        "--metadata",
        str(meta_path),
        "--payload-out",
        str(out_payload),
        "--timeout-s",
        str(timeout_s),
    ]

    # Regeneration must use the manual flowgraph so CRC gating is available and stable.
    # The hierarchical lora_rx block does not expose crc_check, so it can emit payload bytes
    # even when CRC fails (which breaks strict comparisons).
    cmd.append("--force-manual")
    if not bool(meta.get("has_crc", True)):
        # No CRC present: bypass crc_verif to ensure the payload is not blocked.
        cmd.append("--bypass-crc-verif")
    # Guard against rare GNU Radio hangs: ensure one case can't stall regeneration.
    # Add a small grace window for Python/GNU Radio startup overhead.
    try:
        return subprocess.run(cmd, capture_output=True, timeout=float(timeout_s) + 10.0)
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout if exc.stdout is not None else b""
        stderr = exc.stderr if exc.stderr is not None else b""
        stderr = stderr + b"\n[TIMEOUT] GNU decode exceeded timeout\n"
        return subprocess.CompletedProcess(cmd, 124, stdout=stdout, stderr=stderr)


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--in-dir", type=Path, default=DEFAULT_IN_DIR)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument("--gr-python", type=Path, default=DEFAULT_GR_PYTHON)
    parser.add_argument("--timeout-s", type=float, default=60.0)
    parser.add_argument("--max-cases", type=int, default=0)
    args = parser.parse_args(list(argv) if argv is not None else None)

    in_dir = args.in_dir.resolve()
    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    cases = discover_cases(in_dir)
    if args.max_cases and args.max_cases > 0:
        cases = cases[: args.max_cases]

    failures: List[str] = []

    for case in cases:
        summary_json = _load_json(case.summary)
        meta = _build_metadata(case.base, summary_json)

        meta_path = out_dir / f"{case.base}.meta.json"
        meta_path.write_text(json.dumps(meta, indent=2) + "\n")

        out_payload = out_dir / f"{case.base}_gnuradio_payload.bin"
        # Ensure stale output doesn't survive.
        out_payload.write_bytes(b"")

        proc = run_gr_decode(
            gr_python=args.gr_python,
            capture=case.capture,
            meta=meta,
            meta_path=meta_path,
            out_payload=out_payload,
            timeout_s=float(args.timeout_s),
        )
        if proc.returncode != 0:
            failures.append(
                f"{case.base}: gr_decode_capture rc={proc.returncode} stdout={proc.stdout[-200:].decode(errors='ignore')!r} stderr={proc.stderr[-200:].decode(errors='ignore')!r}"
            )

    if failures:
        for line in failures:
            print(line)
        print(f"FAIL ({len(failures)}/{len(cases)} cases)")
        return 1

    print(f"OK ({len(cases)}/{len(cases)} cases) -> {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
