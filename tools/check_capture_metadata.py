#!/usr/bin/env python3
"""
Audit generated capture metadata against the header decoded by lora_replay.

For each metadata JSON in the target directory, run lora_replay with the
associated capture and compare the metadata payload_len/CR to the decoded
header values. Optionally rewrite metadata payload_len when a clean header
decode disagrees.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


HEADER_RE = re.compile(r"Decoded header -> payload_len=(\d+).*cr=(\d+)")
LEN_MISMATCH_RE = re.compile(r"\[header\] payload length (\d+) differs from metadata (\d+)")
CR_MISMATCH_RE = re.compile(r"\[header\] CR (\d+) differs from metadata (\d+)")


@dataclass
class AuditResult:
    capture: Path
    metadata_path: Path
    meta_payload_len: Optional[int]
    meta_cr: Optional[int]
    header_payload_len: Optional[int]
    header_cr: Optional[int]
    status: str
    notes: list[str]
    return_code: int


def parse_output(text: str) -> tuple[Optional[int], Optional[int], list[str]]:
    header_len = None
    header_cr = None
    notes: list[str] = []
    for line in text.splitlines():
        if (m := HEADER_RE.search(line)):
            header_len = int(m.group(1))
            header_cr = int(m.group(2))
        if (m := LEN_MISMATCH_RE.search(line)):
            notes.append(f"payload_len mismatch decoded={m.group(1)} meta={m.group(2)}")
        if (m := CR_MISMATCH_RE.search(line)):
            notes.append(f"cr mismatch decoded={m.group(1)} meta={m.group(2)}")
        if "header decode failed" in line.lower():
            notes.append("header decode failed")
    if header_len is None and not notes:
        notes.append("no header decode line found")
    return header_len, header_cr, notes


def run_lora_replay(lora_replay: Path, capture: Path, metadata: Path) -> tuple[int, str]:
    cmd = [
        str(lora_replay),
        "--iq",
        str(capture),
        "--metadata",
        str(metadata),
        "--allow-stage-mismatch",
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, errors="ignore")
    output = (result.stdout or "") + (result.stderr or "")
    return result.returncode, output


def load_metadata(path: Path) -> dict:
    return json.loads(path.read_text())


def resolve_capture_path(meta: dict, meta_path: Path, captures_dir: Path) -> Path:
    capture_file = meta.get("capture_file") or meta_path.with_suffix(".cf32").name
    return captures_dir / capture_file


def audit_one(lora_replay: Path, meta_path: Path, captures_dir: Path) -> AuditResult:
    meta = load_metadata(meta_path)
    capture_path = resolve_capture_path(meta, meta_path, captures_dir)
    rc, output = run_lora_replay(lora_replay, capture_path, meta_path)
    header_len, header_cr, notes = parse_output(output)
    meta_len = meta.get("payload_len")
    meta_cr = meta.get("cr")

    status = "match"
    if rc != 0:
        status = "error"
        notes.append(f"lora_replay rc={rc}")
    elif header_len is None:
        status = "no_header"
    elif meta_len is None:
        status = "meta_missing"
    elif header_len != meta_len:
        status = "payload_len_mismatch"

    if header_cr is not None and meta_cr is not None and header_cr != meta_cr:
        if status == "match":
            status = "cr_mismatch"
        else:
            notes.append("cr mismatch")

    return AuditResult(
        capture=capture_path,
        metadata_path=meta_path,
        meta_payload_len=meta_len,
        meta_cr=meta_cr,
        header_payload_len=header_len,
        header_cr=header_cr,
        status=status,
        notes=notes,
        return_code=rc,
    )


def write_metadata(path: Path, meta: dict) -> None:
    path.write_text(json.dumps(meta, indent=2) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lora-replay", type=Path, required=True)
    parser.add_argument("--captures-dir", type=Path, default=Path("gr_lora_sdr/data/generated"))
    parser.add_argument("--update", action="store_true", help="Rewrite metadata payload_len to match decoded header")
    parser.add_argument("--update-cr", action="store_true", help="Rewrite metadata CR to match decoded header when available")
    args = parser.parse_args()

    meta_files = sorted(args.captures_dir.glob("*.json"))
    if not meta_files:
        sys.stderr.write(f"No metadata files found in {args.captures_dir}\n")
        return 1

    results: list[AuditResult] = []
    for meta_path in meta_files:
        try:
            result = audit_one(args.lora_replay, meta_path, args.captures_dir)
        except FileNotFoundError as exc:
            results.append(
                AuditResult(
                    capture=Path(""),
                    metadata_path=meta_path,
                    meta_payload_len=None,
                    meta_cr=None,
                    header_payload_len=None,
                    header_cr=None,
                    status="missing_capture",
                    notes=[str(exc)],
                    return_code=1,
                )
            )
            continue
        results.append(result)
        if args.update or args.update_cr:
            meta = load_metadata(meta_path)
            updated = False
            if args.update and result.header_payload_len is not None and result.meta_payload_len != result.header_payload_len:
                meta["payload_len"] = result.header_payload_len
                updated = True
            if args.update_cr and result.header_cr is not None and result.meta_cr != result.header_cr:
                meta["cr"] = result.header_cr
                updated = True
            if updated:
                write_metadata(meta_path, meta)

    mismatches = 0
    print("capture,meta_len,header_len,status,notes")
    for res in results:
        note = "; ".join(res.notes)
        meta_len = res.meta_payload_len if res.meta_payload_len is not None else ""
        header_len = res.header_payload_len if res.header_payload_len is not None else ""
        print(f"{res.capture.name},{meta_len},{header_len},{res.status},{note}")
        if res.status not in ("match",):
            mismatches += 1

    return 0 if mismatches == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
