#!/usr/bin/env python3
"""
Verify that lora_replay's --payload guard still works by running a capture with
a known payload and checking that no mismatch is reported.
"""

from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
from pathlib import Path


def build_bash_literal(hex_string: str) -> str:
    if len(hex_string) % 2 != 0:
        raise ValueError("payload hex must have even length")
    if "00" in hex_string.lower():
        raise ValueError("--payload cannot transport NUL bytes; choose a payload without 0x00")
    chunks = "".join(f"\\x{hex_string[i:i+2]}" for i in range(0, len(hex_string), 2))
    return "$'" + chunks + "'"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lora-replay", required=True, type=Path)
    parser.add_argument("--capture", required=True, type=Path)
    parser.add_argument("--metadata", required=True, type=Path)
    parser.add_argument("--payload-hex", required=True)
    args = parser.parse_args()

    payload_literal = build_bash_literal(args.payload_hex)
    cmd = (
        f"{shlex.quote(str(args.lora_replay))}"
        f" --iq {shlex.quote(str(args.capture))}"
        f" --metadata {shlex.quote(str(args.metadata))}"
        f" --payload {payload_literal}"
    )
    result = subprocess.run(["bash", "-lc", cmd], capture_output=True, text=False)
    output_bytes = (result.stdout or b"") + (result.stderr or b"")
    output = output_bytes.decode("utf-8", errors="ignore")
    if result.returncode != 0:
        sys.stderr.buffer.write(output_bytes)
        raise SystemExit(result.returncode)
    if "[payload] decoded payload differs" in output or "[payload] expected length" in output:
        sys.stderr.write(output)
        raise SystemExit("Payload mismatch reported by lora_replay")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
