#!/usr/bin/env python3
"""
Emit CSV/plot summaries from compare_receivers output.

The goal is to make it easy to visualise telemetry such as CFO/SFO estimates and
streaming chunk timings produced by the C++ receiver (and, when available, the
GNU Radio reference). Metrics are extracted from the `result_json` payload
stored under each backend entry.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

DEFAULT_METRICS = [
    "cfo_initial_hz",
    "cfo_final_hz",
    "sample_rate_ratio_initial",
    "sample_rate_ratio_used",
    "sample_rate_error_ppm",
    "chunk_count",
    "chunk_time_avg_us",
    "chunk_time_max_us",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Summarise compare_receivers metrics")
    parser.add_argument("--input", type=Path, required=True, help="compare_receivers JSON report")
    parser.add_argument(
        "--backend",
        type=str,
        default="cpp_stream",
        help="Backend name to extract metrics from (e.g. cpp_stream, gr_batch)",
    )
    parser.add_argument(
        "--metrics",
        type=str,
        default=",".join(DEFAULT_METRICS),
        help="Comma-separated metric keys (dot-separated for nested fields)",
    )
    parser.add_argument("--csv-out", type=Path, default=Path("results/compare_metrics.csv"))
    parser.add_argument(
        "--plot",
        type=Path,
        default=None,
        help="Optional path to write a matplotlib scatter plot (metric vs index).",
    )
    parser.add_argument(
        "--plot-metric",
        type=str,
        default="cfo_final_hz",
        help="Metric to plot when --plot is provided",
    )
    return parser.parse_args()


def load_results(path: Path) -> Dict[str, Any]:
    data = json.loads(path.read_text())
    if "results" not in data:
        raise ValueError("Input JSON missing 'results' field")
    return data


def split_metrics(metric_str: str) -> List[str]:
    return [m.strip() for m in metric_str.split(",") if m.strip()]


def get_nested_value(obj: Any, path: str) -> Any:
    current = obj
    for part in path.split("."):
        if isinstance(current, dict):
            current = current.get(part)
        elif isinstance(current, list):
            try:
                idx = int(part)
            except ValueError:
                return None
            if idx < 0 or idx >= len(current):
                return None
            current = current[idx]
        else:
            return None
    return current


def extract_metrics(entry: Dict[str, Any], backend: str, metric_keys: Iterable[str]) -> Optional[Dict[str, Any]]:
    backend_result = entry.get("backends", {}).get(backend)
    if not backend_result:
        return None
    extra = backend_result.get("extra", {})
    if not isinstance(extra, dict):
        extra = {}
    result_json = extra.get("result_json")
    if not isinstance(result_json, dict):
        result_json = {}

    row: Dict[str, Any] = {"path": entry.get("path"), "backend": backend}
    for metric in metric_keys:
        value = get_nested_value(result_json, metric)
        if value is None and metric in backend_result:
            value = backend_result.get(metric)
        row[metric] = value
    return row


def write_csv(rows: List[Dict[str, Any]], metrics: List[str], csv_path: Path) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    header = ["path", "backend"] + metrics
    with csv_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=header)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key) for key in header})


def maybe_plot(rows: List[Dict[str, Any]], metric: str, plot_path: Path) -> None:
    try:
        import matplotlib.pyplot as plt  # type: ignore
    except Exception as exc:  # pragma: no cover - matplotlib optional
        print(f"[warn] matplotlib not available ({exc}); skipping plot")
        return

    values = []
    labels = []
    for idx, row in enumerate(rows):
        val = row.get(metric)
        if isinstance(val, (int, float)):
            values.append(val)
            labels.append(idx)
    if not values:
        print(f"[warn] no numeric values for metric '{metric}', skipping plot")
        return
    plt.figure(figsize=(8, 4))
    plt.plot(labels, values, marker="o")
    plt.xlabel("Vector index")
    plt.ylabel(metric)
    plt.title(f"{metric} per vector")
    plt.grid(True, linestyle="--", alpha=0.5)
    plot_path.parent.mkdir(parents=True, exist_ok=True)
    plt.tight_layout()
    plt.savefig(plot_path)
    plt.close()


def main() -> None:
    args = parse_args()
    data = load_results(args.input)
    metrics = split_metrics(args.metrics)
    rows: List[Dict[str, Any]] = []
    for entry in data.get("results", []):
        row = extract_metrics(entry, args.backend, metrics)
        if row is not None:
            rows.append(row)
    if not rows:
        raise SystemExit(f"No rows collected for backend '{args.backend}'")

    write_csv(rows, metrics, args.csv_out.expanduser().resolve())
    print(f"Wrote CSV -> {args.csv_out}")

    if args.plot:
        maybe_plot(rows, args.plot_metric, args.plot.expanduser().resolve())


if __name__ == "__main__":
    main()
