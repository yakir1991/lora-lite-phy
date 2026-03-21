#!/usr/bin/env python3

"""Verify host_sim/lora_replay output against previously saved GNU Radio payload binaries.

This does NOT require GNU Radio to be installed. It re-runs the host receiver for each
capture under build/receiver_vs_gnuradio that has a *_gnuradio_payload.bin reference,
then compares the dumped payload bytes.

Exit code:
  0 - all payloads match
  1 - at least one mismatch or decode failure
"""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_REF_DIR = REPO_ROOT / "build/receiver_vs_gnuradio"


@dataclass
class Case:
    base: str
    capture: Path
    ref_payload: Path
    summary: Path


def _load_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text())


def _find_summary(ref_dir: Path, base: str) -> Optional[Path]:
    direct = ref_dir / f"{base}_standalone_summary.json"
    if direct.exists():
        return direct

    # Some runs produce host_compare summaries (e.g., while debugging).
    host_compare = sorted(ref_dir.glob(f"{base}_host_compare*_summary.json"))
    if host_compare:
        return host_compare[-1]

    # Known short-vector variants share metadata with the non-suffixed case.
    for suffix in ("_2k", "_20k"):
        if base.endswith(suffix):
            fallback = ref_dir / f"{base[: -len(suffix)]}_standalone_summary.json"
            if fallback.exists():
                return fallback

    return None


def discover_cases(ref_dir: Path) -> List[Case]:
    cases: List[Case] = []
    for ref_payload in sorted(ref_dir.glob("*_gnuradio_payload.bin")):
        base = ref_payload.name[: -len("_gnuradio_payload.bin")]
        capture = ref_dir / f"{base}.cf32"
        if not capture.exists():
            raise FileNotFoundError(f"Capture missing for {base}: {capture}")
        summary = _find_summary(ref_dir, base)
        if summary is None:
            raise FileNotFoundError(f"No summary JSON found for {base}")
        cases.append(Case(base=base, capture=capture, ref_payload=ref_payload, summary=summary))
    return cases


def _infer_from_base(meta: Dict[str, Any], base: str) -> None:
    lowered = base.lower()
    if "has_crc" not in meta:
        meta["has_crc"] = "nocrc" not in lowered
    if "implicit_header" not in meta:
        meta["implicit_header"] = "implicithdr" in lowered


def _write_metadata(tmp_dir: Path, base: str, summary_json: Dict[str, Any]) -> Path:
    meta = dict(summary_json.get("metadata", {}))
    _infer_from_base(meta, base)

    # Ensure required booleans are present when missing.
    # If we have SF/BW, infer LDRO per LoRa spec guidance: enable when symbol duration >= 16ms.
    # Ts = 2^SF / BW.
    sf = meta.get("sf")
    bw = meta.get("bw")
    if "ldro" not in meta and isinstance(sf, (int, float)) and isinstance(bw, (int, float)) and bw:
        ts = (2.0 ** float(sf)) / float(bw)
        meta["ldro"] = bool(ts >= 0.016)

    meta.setdefault("has_crc", True)
    meta.setdefault("ldro", False)
    meta.setdefault("implicit_header", False)

    path = tmp_dir / "metadata.json"
    path.write_text(json.dumps(meta, indent=2) + "\n")
    return path


def run_lora_replay(
    lora_replay: Path,
    capture: Path,
    metadata_path: Path,
    out_payload: Path,
    timeout_s: float,
) -> Tuple[int, bytes]:
    cmd = [
        str(lora_replay),
        "--iq",
        str(capture),
        "--metadata",
        str(metadata_path),
        "--dump-payload",
        str(out_payload),
    ]
    proc = subprocess.run(cmd, capture_output=True, timeout=timeout_s)
    return proc.returncode, (proc.stdout or b"") + (proc.stderr or b"")


def compare_bytes(ref_path: Path, test_path: Path) -> Tuple[bool, str]:
    ref_bytes = ref_path.read_bytes()
    test_bytes = test_path.read_bytes() if test_path.exists() else b""

    if ref_bytes == test_bytes:
        return True, ""

    # Find first mismatch for concise reporting.
    max_len = max(len(ref_bytes), len(test_bytes))
    for i in range(max_len):
        rb = ref_bytes[i] if i < len(ref_bytes) else None
        tb = test_bytes[i] if i < len(test_bytes) else None
        if rb != tb:
            return False, f"first_diff@{i}: ref={rb} host={tb} (ref_len={len(ref_bytes)} host_len={len(test_bytes)})"

    return False, f"length_mismatch ref_len={len(ref_bytes)} host_len={len(test_bytes)}"


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref-dir", type=Path, default=DEFAULT_REF_DIR)
    parser.add_argument("--lora-replay", type=Path, default=REPO_ROOT / "build/host_sim/lora_replay")
    parser.add_argument("--timeout-s", type=float, default=180.0)
    parser.add_argument("--max-cases", type=int, default=0, help="Optional cap for quick runs (0 = no cap)")
    args = parser.parse_args(list(argv) if argv is not None else None)

    ref_dir = args.ref_dir.resolve()
    lora_replay = args.lora_replay.resolve()
    if not lora_replay.exists():
        raise FileNotFoundError(f"lora_replay not found: {lora_replay}")

    cases = discover_cases(ref_dir)
    if args.max_cases and args.max_cases > 0:
        cases = cases[: args.max_cases]

    failures: List[str] = []

    for case in cases:
        summary_json = _load_json(case.summary)
        with tempfile.TemporaryDirectory(prefix=f"verify_{case.base}_") as tmp:
            tmp_dir = Path(tmp)
            meta_path = _write_metadata(tmp_dir, case.base, summary_json)
            out_payload = tmp_dir / "host_payload.bin"

            rc, output = run_lora_replay(
                lora_replay=lora_replay,
                capture=case.capture,
                metadata_path=meta_path,
                out_payload=out_payload,
                timeout_s=args.timeout_s,
            )
            if rc != 0:
                failures.append(f"{case.base}: lora_replay rc={rc}")
                continue

            ok, detail = compare_bytes(case.ref_payload, out_payload)
            if not ok:
                failures.append(f"{case.base}: payload_mismatch {detail}")

    if failures:
        for line in failures:
            print(line)
        print(f"FAIL ({len(failures)}/{len(cases)} cases)")
        return 1

    print(f"OK ({len(cases)}/{len(cases)} cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
