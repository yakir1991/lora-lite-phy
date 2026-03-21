#!/usr/bin/env python3
"""Run the SF/CR regression sweep vs. GNU Radio and summarise results.

This helper wraps `tools/run_receiver_vs_gnuradio.py` with the sweep matrix and
then produces a compact Markdown report that highlights payload parity across
all spreading-factor/coding-rate combinations.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MATRIX = REPO_ROOT / "docs" / "receiver_vs_gnuradio_sweep_matrix.json"
DEFAULT_RESULTS = REPO_ROOT / "build" / "receiver_vs_gnuradio_sweep_results.json"
DEFAULT_SUMMARY = REPO_ROOT / "build" / "receiver_vs_gnuradio_sweep_summary.md"
DEFAULT_REPORT = REPO_ROOT / "build" / "receiver_vs_gnuradio_sweep_report.md"
DEFAULT_WORK_DIR = REPO_ROOT / "build" / "receiver_vs_gnuradio_sweep"
DEFAULT_CAPTURE_ROOT = REPO_ROOT / "gr_lora_sdr" / "data" / "generated"
DEFAULT_LORA_REPLAY = REPO_ROOT / "build" / "host_sim" / "lora_replay"
DEFAULT_GR_SCRIPT = REPO_ROOT / "tools" / "gr_decode_capture.py"
CAPTURE_PATTERN = re.compile(r"sf(?P<sf>\d+)_bw(?P<bw>\d+)k_cr(?P<cr>\d+)", re.IGNORECASE)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, default=DEFAULT_MATRIX, help="Sweep matrix JSON path")
    parser.add_argument("--result-json", type=Path, default=DEFAULT_RESULTS, help="Output JSON for raw comparison data")
    parser.add_argument("--summary-md", type=Path, default=DEFAULT_SUMMARY, help="Markdown summary output path")
    parser.add_argument("--report-md", type=Path, default=DEFAULT_REPORT, help="Markdown report emitted by the underlying compare script")
    parser.add_argument("--work-dir", type=Path, default=DEFAULT_WORK_DIR, help="Work directory for impaired IQ + dumps")
    parser.add_argument("--capture-root", type=Path, default=DEFAULT_CAPTURE_ROOT, help="Root folder that holds the .cf32 captures referenced by the matrix")
    parser.add_argument("--lora-replay", type=Path, default=DEFAULT_LORA_REPLAY, help="Path to the standalone decoder binary")
    parser.add_argument("--gnuradio-env", type=str, default=os.environ.get("LORA_CONDA_ENV", "gr310"), help="Conda env that hosts GNU Radio")
    parser.add_argument("--gr-script", type=Path, default=DEFAULT_GR_SCRIPT, help="Helper that decodes captures via GNU Radio")
    parser.add_argument("--captures", nargs="*", help="Optional capture filters (names as listed in the matrix)")
    parser.add_argument("--skip-run", action="store_true", help="Reuse an existing JSON instead of re-running the decode step")
    return parser.parse_args()


def run_compare(args: argparse.Namespace) -> None:
    compare_script = REPO_ROOT / "tools" / "run_receiver_vs_gnuradio.py"
    cmd: List[str] = [
        sys.executable,
        str(compare_script),
        str(args.matrix.resolve()),
        "--output-json",
        str(args.result_json.resolve()),
        "--markdown",
        str(args.report_md.resolve()),
        "--work-dir",
        str(args.work_dir.resolve()),
        "--capture-root",
        str(args.capture_root.resolve()),
        "--lora-replay",
        str(args.lora_replay.resolve()),
        "--gnuradio-env",
        args.gnuradio_env,
        "--gr-script",
        str(args.gr_script.resolve()),
    ]
    if args.captures:
        cmd.append("--captures")
        cmd.extend(args.captures)
    args.work_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(cmd, check=True)


def read_results(path: Path) -> List[Dict[str, object]]:
    data = json.loads(path.read_text())
    if not isinstance(data, list):
        raise RuntimeError(f"Expected a list in {path}")
    return data


def extract_tags(entry: Dict[str, object]) -> Tuple[Optional[int], Optional[int], Optional[int]]:
    capture_name = entry.get("capture") if isinstance(entry, dict) else None
    sf = bw = cr = None
    if isinstance(capture_name, str):
        match = CAPTURE_PATTERN.search(capture_name)
        if match:
            sf = int(match.group("sf"))
            bw = int(match.group("bw"))
            cr = int(match.group("cr"))
    if sf is None or cr is None:
        summary = entry.get("standalone", {}) if isinstance(entry, dict) else {}
        if isinstance(summary, dict):
            meta = summary.get("summary", {}).get("metadata", {})
            if isinstance(meta, dict):
                sf = sf or _safe_int(meta.get("sf"))
                cr = cr or _safe_int(meta.get("cr"))
                bw_hz = meta.get("bw") or meta.get("bw_hz") or meta.get("sample_rate")
                bw = bw or _hz_to_khz(bw_hz)
    return sf, bw, cr


def _safe_int(value: object) -> Optional[int]:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return int(value)
    try:
        return int(str(value))
    except (TypeError, ValueError):
        return None


def _hz_to_khz(value: object) -> Optional[int]:
    if isinstance(value, (int, float)) and value > 0:
        return int(round(value / 1000.0))
    return None


def summarise(results: Iterable[Dict[str, object]], summary_path: Path, matrix: Path, result_json: Path, compare_cmd: Optional[List[str]]) -> None:
    def new_bucket() -> Dict[str, object]:
        return {
            "cases": 0,
            "matches": 0,
            "mismatches": 0,
            "missing": 0,
            "host_fail": 0,
            "gn_fail": 0,
        }

    def new_sfcr_bucket() -> Dict[str, object]:
        bucket = new_bucket()
        bucket["bws"] = set()
        return bucket

    totals = new_bucket()
    per_sf: Dict[Optional[int], Dict[str, object]] = defaultdict(new_bucket)
    per_sf_cr: Dict[Tuple[Optional[int], Optional[int]], Dict[str, object]] = defaultdict(new_sfcr_bucket)
    outstanding: List[Dict[str, object]] = []

    for entry in results:
        sf, bw, cr = extract_tags(entry)
        sf_key = sf
        sfcr_key = (sf, cr)
        bucket_sf = per_sf[sf_key]
        bucket_sfcr = per_sf_cr[sfcr_key]
        bucket_sf["cases"] += 1
        bucket_sfcr["cases"] += 1
        if bw is not None:
            bucket_sfcr["bws"].add(bw)
        totals["cases"] += 1

        host_status = _deep_get(entry, ["standalone", "status"])
        gn_status = _deep_get(entry, ["gnuradio", "status"])
        payload_match = entry.get("payload_match") if isinstance(entry, dict) else None
        payload = entry.get("payload") if isinstance(entry, dict) else None
        expected_hex = payload.get("expected_hex") if isinstance(payload, dict) else None
        host_hex = payload.get("standalone_hex") if isinstance(payload, dict) else None

        issue_reason: Optional[str] = None
        if host_status != "ok":
            bucket_sf["host_fail"] += 1
            bucket_sfcr["host_fail"] += 1
            totals["host_fail"] += 1
            issue_reason = f"standalone status {host_status}"
        elif gn_status != "ok":
            bucket_sf["gn_fail"] += 1
            bucket_sfcr["gn_fail"] += 1
            totals["gn_fail"] += 1
            issue_reason = f"GNU Radio status {gn_status}"
        elif payload_match is True:
            bucket_sf["matches"] += 1
            bucket_sfcr["matches"] += 1
            totals["matches"] += 1
        else:
            if expected_hex is None or host_hex is None:
                bucket_sf["missing"] += 1
                bucket_sfcr["missing"] += 1
                totals["missing"] += 1
                if expected_hex is None and host_hex is None:
                    issue_reason = "both decoders missing payload"
                elif expected_hex is None:
                    issue_reason = "GNU Radio missing payload"
                else:
                    issue_reason = "standalone missing payload"
            else:
                bucket_sf["mismatches"] += 1
                bucket_sfcr["mismatches"] += 1
                totals["mismatches"] += 1
                issue_reason = "payload mismatch"

        if issue_reason:
            outstanding.append(
                {
                    "capture": entry.get("capture"),
                    "sf": sf,
                    "cr": cr,
                    "bw": bw,
                    "reason": issue_reason,
                    "standalone": host_status,
                    "gnuradio": gn_status,
                }
            )

    lines: List[str] = []
    lines.append("# SF/CR Regression Summary")
    lines.append("")
    if compare_cmd:
        quoted = " ".join(shlex.quote(part) for part in compare_cmd)
        lines.append(f"- Command: `{quoted}`")
    lines.append(f"- Matrix: `{matrix}`")
    lines.append(f"- Raw results: `{result_json}`")
    lines.append("")
    lines.append("## Totals")
    lines.append("")
    lines.append(
        f"- Cases: {totals['cases']}, Matches: {totals['matches']}, "
        f"Mismatches: {totals['mismatches']}, Missing Payload: {totals['missing']}, "
        f"Standalone Failures: {totals['host_fail']}, GNU Radio Failures: {totals['gn_fail']}"
    )
    lines.append("")

    def build_table(headers: List[str], rows: List[List[str]]) -> List[str]:
        table = ["| " + " | ".join(headers) + " |", "| " + " | ".join("---" for _ in headers) + " |"]
        for row in rows:
            table.append("| " + " | ".join(row) + " |")
        return table

    lines.append("## Breakdown by SF")
    lines.append("")
    sf_rows: List[List[str]] = []
    for sf_key in sorted(per_sf.keys(), key=lambda k: (k is None, k if k is not None else 0)):
        bucket = per_sf[sf_key]
        label = f"SF{sf_key}" if sf_key is not None else "Unknown"
        sf_rows.append(
            [
                label,
                str(bucket["cases"]),
                str(bucket["matches"]),
                str(bucket["mismatches"]),
                str(bucket["missing"]),
                str(bucket["host_fail"]),
                str(bucket["gn_fail"]),
            ]
        )
    lines.extend(build_table(["SF", "Cases", "Matches", "Mismatches", "Missing", "Standalone Fail", "GNU Radio Fail"], sf_rows))
    lines.append("")

    lines.append("## Breakdown by SF/CR")
    lines.append("")
    sfcr_rows: List[List[str]] = []
    for key in sorted(per_sf_cr.keys(), key=lambda k: ((k[0] is None), k[0] if k[0] is not None else 0, k[1] if k[1] is not None else 0)):
        sf_val, cr_val = key
        bucket = per_sf_cr[key]
        bw_list = sorted(bucket["bws"])
        bw_text = ", ".join(f"{bw} kHz" for bw in bw_list) if bw_list else "-"
        sfcr_rows.append(
            [
                f"SF{sf_val}" if sf_val is not None else "?",
                f"CR{cr_val}" if cr_val is not None else "?",
                str(bucket["cases"]),
                str(bucket["matches"]),
                str(bucket["mismatches"]),
                str(bucket["missing"]),
                str(bucket["host_fail"]),
                str(bucket["gn_fail"]),
                bw_text,
            ]
        )
    lines.extend(
        build_table(
            [
                "SF",
                "CR",
                "Cases",
                "Matches",
                "Mismatches",
                "Missing",
                "Standalone Fail",
                "GNU Radio Fail",
                "BWs",
            ],
            sfcr_rows,
        )
    )
    lines.append("")

    if outstanding:
        lines.append("## Outstanding Items")
        lines.append("")
        for item in outstanding:
            label = item.get("capture") or "(unknown capture)"
            reason = item.get("reason", "unknown issue")
            sf_text = f"SF{item['sf']}" if item.get("sf") is not None else "SF?"
            cr_text = f"CR{item['cr']}" if item.get("cr") is not None else "CR?"
            bw_text = f"{item['bw']} kHz" if item.get("bw") is not None else "BW?"
            lines.append(
                f"- `{label}` ({sf_text}, {cr_text}, {bw_text}): {reason}; "
                f"standalone={item.get('standalone')}, gnuradio={item.get('gnuradio')}"
            )
        lines.append("")
    else:
        lines.append("## Outstanding Items")
        lines.append("")
        lines.append("- None – all payloads matched")

    summary_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.write_text("\n".join(lines) + "\n")
    print(f"Wrote summary to {summary_path}")


def _deep_get(root: object, path: List[str]) -> Optional[str]:
    node = root
    for key in path:
        if not isinstance(node, dict):
            return None
        node = node.get(key)
    return node if isinstance(node, str) else None


def main() -> int:
    args = parse_args()
    matrix = args.matrix.resolve()
    if not matrix.exists():
        raise FileNotFoundError(f"Matrix not found: {matrix}")
    compare_cmd: Optional[List[str]] = None
    if not args.skip_run:
        run_compare(args)
        compare_cmd = [
            str(sys.executable),
            str((REPO_ROOT / "tools" / "run_receiver_vs_gnuradio.py").resolve()),
            str(matrix),
            "--output-json",
            str(args.result_json.resolve()),
            "--markdown",
            str(args.report_md.resolve()),
            "--work-dir",
            str(args.work_dir.resolve()),
            "--capture-root",
            str(args.capture_root.resolve()),
            "--lora-replay",
            str(args.lora_replay.resolve()),
            "--gnuradio-env",
            args.gnuradio_env,
            "--gr-script",
            str(args.gr_script.resolve()),
        ]
        if args.captures:
            compare_cmd.append("--captures")
            compare_cmd.extend(args.captures)
    else:
        if not args.result_json.exists():
            raise FileNotFoundError("--skip-run requested but result JSON is missing")
    results = read_results(args.result_json)
    summarise(results, args.summary_md, matrix, args.result_json.resolve(), compare_cmd)
    print(f"Raw comparison JSON: {args.result_json}")
    print(f"Detailed report: {args.report_md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
