#!/usr/bin/env python3
"""Validate header/payload boundaries and CRC handling across reference vectors."""

import json
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CLI_PATH = REPO_ROOT / "standalone" / "build" / "lora_rx"


CASES = [
    {
        "file": REPO_ROOT
        / "vectors"
        / "sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown",
        "sf": 7,
        "bw": 125000,
        "fs": 500000,
    }
]


def run_case(case: dict) -> dict:
    cmd = [
        str(CLI_PATH),
        "--json",
        "--debug-crc",
        str(case["file"]),
        str(case["sf"]),
        str(case["bw"]),
        str(case["fs"]),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    try:
        data = json.loads(result.stdout.strip())
    except json.JSONDecodeError as exc:  # pragma: no cover - defensive guard
        raise RuntimeError(f"Failed to parse JSON output for {case['file']}: {exc}\n{result.stdout}") from exc
    return data


def validate(data: dict) -> None:
    payload_len = data.get("payload_len", -1)
    payload_bytes = data.get("payload_bytes", [])
    has_crc = bool(data.get("has_crc", False))
    expected = payload_len + (2 if has_crc else 0)
    assert len(payload_bytes) == expected, (
        f"payload byte count mismatch: expected {expected}, got {len(payload_bytes)}"
    )
    if has_crc:
        assert data.get("payload_crc_semtech_ok", False), "Semtech CRC check failed"
        trace = data.get("payload_crc_trace", [])
        assert len(trace) == len(payload_bytes), "CRC trace length mismatch"
        prns = data.get("payload_whitening_prns", [])
        assert len(prns) == payload_len, "Whitening sequence length mismatch"


def main() -> int:
    if not CLI_PATH.exists():
        print(f"CLI not found at {CLI_PATH}. Build the standalone project first.", file=sys.stderr)
        return 2
    any_fail = False
    for case in CASES:
        data = run_case(case)
        try:
            validate(data)
        except AssertionError as exc:
            any_fail = True
            print(f"[FAIL] {case['file'].name}: {exc}")
            continue
        msg = "ok" if data.get("payload_crc_semtech_ok") else "crc-fail"
        print(
            f"[OK] {case['file'].name}: payload_len={data.get('payload_len')} bytes={len(data.get('payload_bytes', []))} ({msg})"
        )
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())
