#!/usr/bin/env python3

"""
Run real-time/impairment stress sweeps and summarise results to JSON.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass
class SweepCase:
    capture: Path
    metadata: Path
    impair_flags: list[str]
    rt_flags: list[str]
    label: str
    display_impair_flags: list[str]
    display_rt_flags: list[str]
    allow_tracking_failure: bool

    def summary_path(self, output_dir: Path) -> Path:
        name = self.capture.stem
        label_str = self.label.replace(" ", "_") if self.label else "case"
        return output_dir / f"{label_str}_{name}.json"


def run_case(lora_replay: Path, case: SweepCase, summary_path: Path, allow_tracking_failure: bool) -> dict:
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(lora_replay),
        "--iq",
        str(case.capture),
        "--metadata",
        str(case.metadata),
        "--summary",
        str(summary_path),
    ]
    cmd.extend(case.impair_flags)
    cmd.extend(case.rt_flags)

    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"lora_replay failed ({result.returncode}): {' '.join(cmd)}")

    summary = json.loads(summary_path.read_text())
    summary["summary_path"] = str(summary_path)
    summary["label"] = case.label

    if summary.get("tracking_failure"):
        reason = summary.get("tracking_failure_reason", "unknown")
        mitigation = summary.get("tracking_mitigation", "")
        message = f"[warn] tracking failure for {case.capture} ({reason})"
        if mitigation:
            message += f" – suggested mitigation: {mitigation}"
        print(message, file=sys.stderr)
        if not allow_tracking_failure:
            raise RuntimeError(f"tracking failure encountered for {case.capture}")

    return summary


def _resolve_waveform_path(waveform: str, matrix_dir: Path, data_dir: Path) -> Path:
    """
    Resolve a collision waveform path allowing both relative-to-matrix and repo-root references.
    """
    candidate = Path(waveform)
    if candidate.is_absolute() and candidate.exists():
        return candidate

    probe_paths = [
        (matrix_dir / candidate).resolve(),
        (matrix_dir.parent / candidate).resolve(),
        (data_dir / candidate).resolve(),
    ]
    for path in probe_paths:
        if path.exists():
            return path

    raise FileNotFoundError(f"Collision waveform not found for entry '{waveform}' (searched {probe_paths})")


def build_cases(data_dir: Path, manifest: Iterable[dict], matrix: dict, matrix_dir: Path) -> list[SweepCase]:
    cases: list[SweepCase] = []
    rt_speed = matrix.get("rt_speed", 1.0)
    impairments = matrix.get("impairments", [])
    capture_allow_set = {Path(item).name for item in matrix.get("allow_tracking_failure_captures", [])}
    for entry in manifest:
        capture = data_dir / entry["capture"]
        metadata = capture.with_suffix(".json")
        for impair in impairments:
            flags: list[str] = []
            flags_display: list[str] = []
            if "cfo_ppm" in impair:
                payload = ["--impair-cfo-ppm", str(impair["cfo_ppm"])]
                flags.extend(payload)
                flags_display.extend(payload)
            if "sfo_ppm" in impair:
                payload = ["--impair-sfo-ppm", str(impair["sfo_ppm"])]
                flags.extend(payload)
                flags_display.extend(payload)
            if "cfo_drift_ppm" in impair:
                payload = ["--impair-cfo-drift-ppm", str(impair["cfo_drift_ppm"])]
                flags.extend(payload)
                flags_display.extend(payload)
            if "sfo_drift_ppm" in impair:
                payload = ["--impair-sfo-drift-ppm", str(impair["sfo_drift_ppm"])]
                flags.extend(payload)
                flags_display.extend(payload)
            if "awgn_snr_db" in impair:
                payload = ["--impair-awgn-snr", str(impair["awgn_snr_db"])]
                flags.extend(payload)
                flags_display.extend(payload)
            if "burst" in impair:
                burst = impair["burst"]
                payload = [
                    "--impair-burst-period",
                    str(burst.get("period", 0)),
                    "--impair-burst-duration",
                    str(burst.get("duration", 0)),
                    "--impair-burst-snr",
                    str(burst.get("snr_db", 0)),
                ]
                flags.extend(payload)
                flags_display.extend(payload)
            if "collision" in impair:
                collision = impair["collision"]
                waveform = collision.get("waveform")
                if waveform:
                    waveform_path = _resolve_waveform_path(waveform, matrix_dir, data_dir)
                    payload_runtime = [
                        "--impair-collision-prob",
                        str(collision.get("prob", collision.get("probability", 0.0))),
                        "--impair-collision-scale",
                        str(collision.get("scale", 1.0)),
                        "--impair-collision-file",
                        str(waveform_path),
                    ]
                    payload_display = payload_runtime.copy()
                    payload_display[-1] = str(waveform)
                    flags.extend(payload_runtime)
                    flags_display.extend(payload_display)
            seed_payload = ["--impair-seed", str(impair.get("seed", 0))]
            flags.extend(seed_payload)
            flags_display.extend(seed_payload)

            label = impair.get("label", f"{entry['capture']}_case")
            rt_flags = ["--rt", "--rt-speed", str(rt_speed)]
            rt_flags_display = rt_flags.copy()
            allow_tracking_failure = bool(
                impair.get("allow_tracking_failure", False)
                or Path(entry["capture"]).name in capture_allow_set
            )
            cases.append(
                SweepCase(
                    capture=capture,
                    metadata=metadata,
                    impair_flags=flags,
                    rt_flags=rt_flags,
                    label=label,
                    display_impair_flags=flags_display,
                    display_rt_flags=rt_flags_display,
                    allow_tracking_failure=allow_tracking_failure,
                )
            )
    return cases


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="JSON array of capture entries (same schema as reference manifest)")
    parser.add_argument("data_dir", type=Path, help="Directory containing capture .cf32/.json pairs")
    parser.add_argument("output", type=Path, help="Output JSON summarising the sweep")
    parser.add_argument("--lora-replay", type=Path, default=Path("host_sim/lora_replay"))
    parser.add_argument("--matrix", type=Path, required=True, help="Sweep matrix JSON (impairments, rt_speed, etc.)")
    parser.add_argument("--summary-dir", type=Path, default=Path("build/stage4_summaries"))
    parser.add_argument(
        "--allow-tracking-failures",
        action="store_true",
        help="Do not fail the sweep when tracking_failure is reported in a summary",
    )
    args = parser.parse_args()

    lora_replay = args.lora_replay.resolve()
    if not lora_replay.exists():
        raise FileNotFoundError(f"lora_replay not found: {lora_replay}")

    manifest_entries = json.loads(args.manifest.read_text())
    matrix = json.loads(args.matrix.read_text())

    cases = build_cases(args.data_dir, manifest_entries, matrix, args.matrix.parent)
    results = []
    for case in cases:
        summary_path = case.summary_path(args.summary_dir)
        allow_failure = args.allow_tracking_failures or case.allow_tracking_failure
        summary = run_case(lora_replay, case, summary_path, allow_failure)
        results.append(
            {
                "label": summary.get("label", case.label),
                "capture": str(case.capture),
                "metadata": str(case.metadata),
                "impair_flags": case.display_impair_flags,
                "rt_flags": case.display_rt_flags,
                "summary_path": summary.get("summary_path", str(summary_path)),
                "allow_tracking_failure": allow_failure,
                "tracking_failure": summary.get("tracking_failure", False),
                "tracking_failure_reason": summary.get("tracking_failure_reason"),
                "tracking_mitigation": summary.get("tracking_mitigation"),
                "metrics": {
                    "acquisition_time_us": summary.get("acquisition_time_us"),
                    "tracking_jitter_us": summary.get("tracking_jitter_us"),
                    "packet_error_rate": summary.get("packet_error_rate"),
                    "bit_error_rate": summary.get("bit_error_rate"),
                    "deadline_miss_count": summary.get("deadline_miss_count"),
                    "rt_overrun_count": summary.get("rt_overrun_count"),
                    "rt_underrun_count": summary.get("rt_underrun_count"),
                },
            }
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
