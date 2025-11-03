#!/usr/bin/env python3

"""Compare LoRa replay summary metrics against a baseline with tolerances."""

from __future__ import annotations

import argparse
import json
import math
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


@dataclass
class MetricSpec:
    name: str
    value: Any
    tolerance: float = 0.0
    mode: str = "exact"  # "exact", "relative", "absolute"


def parse_metric_spec(name: str, payload: Any) -> MetricSpec:
    if isinstance(payload, dict):
        value = payload.get("value")
        tolerance = float(payload.get("tolerance", 0.0))
        mode = payload.get("mode", "exact")
        return MetricSpec(name=name, value=value, tolerance=tolerance, mode=mode)
    return MetricSpec(name=name, value=payload)


def load_baseline(path: Path) -> Dict[str, List[MetricSpec]]:
    data = json.loads(path.read_text())
    result: Dict[str, List[MetricSpec]] = {}
    for entry in data:
        capture = entry["capture"]
        metrics_payload = entry.get("metrics", {})
        specs = [parse_metric_spec(name, spec) for name, spec in metrics_payload.items()]
        result[capture] = specs
    return result


def compute_summary_metrics(summary: Dict[str, Any]) -> Dict[str, Any]:
    timings = summary.get("stage_timings_ns", [])
    metrics: Dict[str, Any] = {
        "reference_mismatches": summary.get("reference_mismatches"),
        "stage_mismatches": summary.get("stage_mismatches"),
        "whitening_roundtrip_ok": summary.get("whitening_roundtrip_ok"),
        "symbols": len(timings),
    }
    if timings:
        timings_sorted = sorted(timings)
        avg = statistics.fmean(timings)
        p95_index = max(0, int(0.95 * (len(timings_sorted) - 1)))
        metrics["stage_timing_avg_ns"] = avg
        metrics["stage_timing_p95_ns"] = timings_sorted[p95_index]
    else:
        metrics["stage_timing_avg_ns"] = 0.0
        metrics["stage_timing_p95_ns"] = 0.0
    return metrics


def locate_summary(summary_dir: Path, capture_name: str) -> Path:
    base = Path(capture_name).name
    summary_name = base.replace(".cf32", ".json")
    return summary_dir / summary_name


def compare_metric(spec: MetricSpec, actual: Any) -> Optional[str]:
    if spec.mode == "exact":
        if actual != spec.value:
            return f"expected {spec.value!r}, got {actual!r}"
        return None

    if spec.mode == "absolute":
        diff = abs(float(actual) - float(spec.value))
        if diff > spec.tolerance:
            return f"abs diff {diff:.6g} exceeds tolerance {spec.tolerance:.6g}"
        return None

    if spec.mode == "relative":
        expected = float(spec.value)
        actual_val = float(actual)
        if math.isclose(expected, 0.0):
            diff = abs(actual_val - expected)
            if diff > spec.tolerance:
                return f"abs diff {diff:.6g} exceeds tolerance {spec.tolerance:.6g} around zero"
            return None
        rel = abs(actual_val - expected) / abs(expected)
        if rel > spec.tolerance:
            return f"relative diff {rel:.6%} exceeds tolerance {spec.tolerance:.6%}"
        return None

    return f"unknown comparison mode {spec.mode!r}"


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path, help="Path to baseline metrics JSON")
    parser.add_argument(
        "summary_dir",
        type=Path,
        help="Directory containing summary JSON files produced by lora_replay",
    )
    args = parser.parse_args(argv)

    baseline = load_baseline(args.baseline)
    failures: List[str] = []

    for capture, specs in baseline.items():
        summary_path = locate_summary(args.summary_dir, capture)
        if not summary_path.exists():
            failures.append(f"summary file missing for {capture}: {summary_path}")
            continue

        summary_data = json.loads(summary_path.read_text())
        actual_metrics = compute_summary_metrics(summary_data)

        for spec in specs:
            if spec.name not in actual_metrics:
                failures.append(f"metric {spec.name} missing in summary for {capture}")
                continue
            actual_value = actual_metrics[spec.name]
            error = compare_metric(spec, actual_value)
            if error:
                failures.append(
                    f"{capture}: metric {spec.name} {error} (baseline={spec.value}, actual={actual_value})"
                )

    if failures:
        print("Summary comparison failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("Summary metrics match baseline within tolerance.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
