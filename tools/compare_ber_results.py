#!/usr/bin/env python3

"""
Compare BER sweep results against a baseline with tolerances.
Baseline/result JSON format follows tools/run_ber_sweep.py aggregated output.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Dict, Iterable, Optional, Tuple

MetricKey = Tuple[str, float]  # (capture, snr_db)


def load_results(path: Path) -> Dict[MetricKey, Dict[str, float]]:
    data = json.loads(path.read_text())
    results: Dict[MetricKey, Dict[str, float]] = {}
    for entry in data:
        capture = entry["capture"]
        for point in entry.get("snr_points", []):
            snr = float(point["snr_db"])
            metrics = {
                "avg_packet_error_rate": float(point.get("avg_packet_error_rate", 0.0)),
                "avg_bit_error_rate": float(point.get("avg_bit_error_rate", 0.0)),
                "max_packet_error_rate": float(point.get("max_packet_error_rate", 0.0)),
                "max_bit_error_rate": float(point.get("max_bit_error_rate", 0.0)),
                "avg_reference_mismatches": float(point.get("avg_reference_mismatches", 0.0)),
                "max_reference_mismatches": float(point.get("max_reference_mismatches", 0.0)),
            }
            results[(capture, snr)] = metrics
    return results


def compare_metric(expected: float, actual: float, abs_tol: float, rel_tol: float) -> Optional[str]:
    diff = abs(actual - expected)
    if diff <= abs_tol:
        return None
    if math.isclose(expected, 0.0):
        if diff > abs_tol:
            return f"abs diff {diff:.3e} exceeds tolerance {abs_tol:.3e}"
        return None
    rel = diff / abs(expected)
    if rel > rel_tol:
        return f"relative diff {rel:.3%} exceeds tolerance {rel_tol:.3%}"
    return None


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("results", type=Path)
    parser.add_argument("--abs-tol", type=float, default=5e-3, help="Absolute tolerance for BER/PER comparisons")
    parser.add_argument("--rel-tol", type=float, default=0.1, help="Relative tolerance for BER/PER comparisons")
    args = parser.parse_args(argv)

    baseline = load_results(args.baseline)
    results = load_results(args.results)

    failures = []

    for key, expected_metrics in baseline.items():
        if key not in results:
            failures.append(f"{key[0]}@SNR{key[1]:.1f}: missing results")
            continue
        actual_metrics = results[key]
        for metric in [
            "avg_packet_error_rate",
            "avg_bit_error_rate",
            "max_packet_error_rate",
            "max_bit_error_rate",
        ]:
            expected = expected_metrics.get(metric, 0.0)
            actual = actual_metrics.get(metric, 0.0)
            error = compare_metric(expected, actual, args.abs_tol, args.rel_tol)
            if error:
                failures.append(
                    f"{key[0]}@SNR{key[1]:.1f}: {metric} {error} (baseline={expected:.6e}, actual={actual:.6e})"
                )

    if failures:
        print("BER sweep comparison failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("BER sweep results within tolerances.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
