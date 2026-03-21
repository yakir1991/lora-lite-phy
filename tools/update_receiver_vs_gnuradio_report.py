#!/usr/bin/env python3
"""Regenerate docs/receiver_vs_gnuradio_report.md from the consolidated results JSON."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, List, Tuple

REPO_ROOT = Path(__file__).resolve().parents[1]
RESULTS_PATH = REPO_ROOT / "docs" / "receiver_vs_gnuradio_results.json"
REPORT_PATH = REPO_ROOT / "docs" / "receiver_vs_gnuradio_report.md"


def load_results(path: Path) -> List[Dict[str, Any]]:
    data = json.loads(path.read_text())
    data.sort(key=lambda item: (
        item.get("capture", ""),
        item.get("profile", ""),
        item.get("mc_iteration", 0),
        item.get("mc_seed", 0),
    ))
    return data


def extract_impairments(entry: Dict[str, Any]) -> Tuple[float, int, float]:
    imp = entry.get("impairment") or {}
    cfo = float(imp.get("cfo_ppm") or 0.0)
    sto = int(imp.get("sto_samples") or 0)
    sfo = float(imp.get("sfo_ppm") or 0.0)
    return cfo, sto, sfo


def status_label(metrics: Dict[str, Any], payload_hex: str | None) -> str:
    status = metrics.get("status", "unknown")
    if status != "ok":
        return status
    return "ok" if payload_hex else "no-payload"


def format_ms(seconds: float) -> str:
    return f"{seconds * 1_000:.1f}"


def build_report(rows: List[Dict[str, Any]], output_path: Path) -> None:
    lines: List[str] = []
    lines.append("# Standalone vs GNU Radio Comparison")
    lines.append("")
    lines.append("| Capture | Profile | CFO ppm | STO samples | SFO ppm | GNU Radio (ms) | Standalone (ms) | Notes |")
    lines.append("| --- | --- | ---:| ---:| ---:| ---:| ---:| --- |")

    for item in rows:
        capture = item.get("capture", "?")
        profile = item.get("profile", "?")
        cfo, sto, sfo = extract_impairments(item)
        gn = item.get("gnuradio", {})
        host = item.get("standalone", {})
        payload = item.get("payload") or {}
        gn_ms = format_ms(float(gn.get("wall_s") or 0.0))
        host_ms = format_ms(float(host.get("wall_s") or 0.0))
        gn_status = status_label(gn, payload.get("expected_hex"))
        host_status = status_label(host, payload.get("standalone_hex"))
        notes = f"{gn_status}/{host_status}"
        lines.append(
            f"| {capture} | {profile} | {cfo:+.1f} | {sto} | {sfo:+.1f} | {gn_ms} | {host_ms} | {notes} |"
        )

    output_path.write_text("\n".join(lines) + "\n")
    print(f"Wrote markdown report with {len(rows)} rows -> {output_path}")


def main() -> None:
    results = load_results(RESULTS_PATH)
    if not results:
        raise SystemExit("No rows found in results JSON; run the comparisons first.")
    build_report(results, REPORT_PATH)


if __name__ == "__main__":
    main()

