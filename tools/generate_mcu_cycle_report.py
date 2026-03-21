#!/usr/bin/env python3

"""Produce a markdown report summarising MCU cycle utilisation from lora_replay summaries."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict, List


def load_summary(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text())


def format_percentage(value: float) -> str:
    return f"{value * 100.0:.2f}%"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary_dir", type=Path, help="Directory containing summary JSON files")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("docs/stage3_mcu_cycle_report.md"),
        help="Markdown output path",
    )
    parser.add_argument(
        "--mode",
        action="append",
        default=[],
        help="Filter by instrumentation mode (repeatable)",
    )
    args = parser.parse_args()

    summary_files = sorted(args.summary_dir.glob("*.json"))
    if not summary_files:
        raise FileNotFoundError(f"No summary JSON files in {args.summary_dir}")

    requested_modes = set(args.mode)
    sections: Dict[str, List[Dict[str, Any]]] = {}

    for path in summary_files:
        data = load_summary(path)
        mode = data.get("instrumentation_numeric_mode", "unknown")
        if requested_modes and mode not in requested_modes:
            continue
        sections.setdefault(mode, []).append(data)

    lines: List[str] = []
    lines.append("# Stage 3 MCU Cycle Report")
    lines.append("")
    lines.append("This report is auto-generated from `lora_replay` summary outputs.")
    lines.append("")

    for mode in sorted(sections.keys()):
        entries = sections[mode]
        if not entries:
            continue
        lines.append(f"## Instrumentation Mode: `{mode}`")
        lines.append("")
        header = (
            "| Capture | MCU | Budget Cycles | Avg Cycles | Max Cycles | Util Avg | Util Max | "
            "Max Symbol Memory (bytes) | Stage Count | Symbols | Min Margin (us) | Deadline Misses |"
        )
        lines.append(header)
        lines.append(
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
        )
        for data in sorted(entries, key=lambda d: d.get("capture", "")):
            capture = Path(data.get("capture", "?")).name
            stage_count = data.get("stage_count", 0)
            symbols = data.get("instrumented_symbols", 0)
            max_symbol_memory = data.get("max_symbol_memory_bytes", 0)
            min_margin_ns = data.get("min_deadline_margin_ns", 0.0)
            min_margin_us = min_margin_ns / 1000.0 if min_margin_ns else 0.0
            deadline_miss = data.get("deadline_miss_count", 0)
            mcu_entries = data.get("mcu_cycle_summaries", [])
            if not mcu_entries:
                lines.append(
                    f"| {capture} | (none) | - | - | - | - | - | {max_symbol_memory} | {stage_count} | {symbols} | "
                    f"{min_margin_us:.2f} | {deadline_miss} |"
                )
                continue
            for entry in mcu_entries:
                name = entry.get("name", "?")
                budget = entry.get("cycles_per_symbol_budget", 0.0)
                avg = entry.get("cycles_per_symbol_avg", 0.0)
                max_cycles = entry.get("cycles_per_symbol_max", 0.0)
                util_avg = entry.get("utilisation_avg", 0.0)
                util_max = entry.get("utilisation_max", 0.0)
                budget_str = f"{budget:,.0f}" if budget else "-"
                avg_str = f"{avg:,.0f}" if avg else "-"
                max_str = f"{max_cycles:,.0f}" if max_cycles else "-"
                util_avg_str = format_percentage(util_avg) if budget else "-"
                util_max_str = format_percentage(util_max) if budget else "-"
                lines.append(
                    "| {capture} | {name} | {budget} | {avg} | {max} | {util_avg} | {util_max} | "
                    "{mem} | {stages} | {symbols} | {margin:.2f} | {misses} |".format(
                        capture=capture,
                        name=name,
                        budget=budget_str,
                        avg=avg_str,
                        max=max_str,
                        util_avg=util_avg_str,
                        util_max=util_max_str,
                        mem=max_symbol_memory,
                        stages=stage_count,
                        symbols=symbols,
                        margin=min_margin_us,
                        misses=deadline_miss,
                    )
                )
        lines.append("")

    output = args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines))
    print(f"Wrote MCU cycle report to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
