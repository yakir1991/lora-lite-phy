#!/usr/bin/env python3
"""Cross-check diagonal deinterleaver output against a Python reference."""

import json
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CLI_PATH = REPO_ROOT / "standalone" / "build" / "lora_rx"


CASES = [
    {
        "file": REPO_ROOT / "vectors" / "test_short_payload.unknown",
        "sf": 7,
        "bw": 125000,
        "fs": 250000,
    },
    {
        "file": REPO_ROOT
        / "vectors"
        / "sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown",
        "sf": 7,
        "bw": 125000,
        "fs": 500000,
    },
]


def run_case(case: dict) -> dict:
    cmd = [
        str(CLI_PATH),
        "--json",
        "--dump-bits",
        str(case["file"]),
        str(case["sf"]),
        str(case["bw"]),
        str(case["fs"]),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    try:
        return json.loads(result.stdout.strip())
    except json.JSONDecodeError as exc:  # pragma: no cover - defensive guard
        raise RuntimeError(f"Failed to parse JSON for {case['file']}: {exc}\n{result.stdout}") from exc


def diag_deinterleave(rows: int, cols: int, inter_bits: list[int]) -> list[int]:
    expected = [0] * (rows * cols)
    for col in range(cols):
        for row in range(rows):
            dest = (col - row - 1) % rows
            expected[dest * cols + col] = inter_bits[col * rows + row]
    return expected


def validate_header(data: dict) -> None:
    rows = data.get("header_rows", 0)
    cols = data.get("header_cols", 0)
    if not rows or not cols:
        return
    inter = data.get("header_inter_bits", [])
    deinter = data.get("header_deinter_bits", [])
    assert len(inter) == rows * cols, "Header interleaver size mismatch"
    expected = diag_deinterleave(rows, cols, inter)
    assert expected == deinter, "Header deinterleaver mismatch"


def validate_payload(data: dict) -> None:
    blocks = data.get("payload_blocks", [])
    for idx, block in enumerate(blocks):
        rows = block.get("rows", 0)
        cols = block.get("cols", 0)
        inter = block.get("inter_bits", [])
        deinter = block.get("deinter_bits", [])
        assert len(inter) == rows * cols, f"Payload block {idx} inter size mismatch"
        expected = diag_deinterleave(rows, cols, inter)
        assert expected == deinter, f"Payload block {idx} mismatch"


def main() -> int:
    if not CLI_PATH.exists():
        print(f"CLI not found at {CLI_PATH}. Build the standalone project first.", file=sys.stderr)
        return 2
    any_fail = False
    for case in CASES:
        data = run_case(case)
        try:
            validate_header(data)
            validate_payload(data)
        except AssertionError as exc:
            any_fail = True
            print(f"[FAIL] {case['file'].name}: {exc}")
            continue
        print(
            f"[OK] {case['file'].name}: header rows={data.get('header_rows')} payload blocks={len(data.get('payload_blocks', []))}"
        )
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())
