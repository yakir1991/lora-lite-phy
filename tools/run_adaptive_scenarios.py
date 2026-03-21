#!/usr/bin/env python3

"""Run adaptive scenarios (multi-step SF/BW sweeps) and summarise results."""

from __future__ import annotations

import argparse
import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Tuple


@dataclass
class Step:
    capture: Path
    metadata: Path
    impair_flags: list[str]
    rt_flags: list[str]
    label: str
    expect_fallback: bool = False


def determine_fallback(summary: Dict[str, Any]) -> Tuple[bool, List[str], str]:
    triggered = False
    reasons: List[str] = []
    actions: List[str] = []

    if summary.get("tracking_failure"):
        triggered = True
        reasons.append("tracking_failure")
        mitigation = summary.get("tracking_mitigation")
        if mitigation:
            actions.append(str(mitigation))
        else:
            actions.append("Re-run frequency/time acquisition or extend preamble")

    timing_violation = (
        summary.get("deadline_miss_count", 0) > 0
        or summary.get("rt_overrun_count", 0) > 0
        or summary.get("rt_min_deadline_margin_ns", 0.0) < 0.0
    )
    if timing_violation:
        triggered = True
        reasons.append("timing_violation")
        actions.append("Reduce rt-speed or widen DMA/ISR tolerances")

    stage_mismatches = summary.get("stage_mismatches", 0)
    if stage_mismatches and stage_mismatches > 0:
        triggered = True
        reasons.append("stage_mismatches")
        actions.append("Re-run alignment or rebaseline stage comparisons")

    deduped_actions: List[str] = []
    for act in actions:
        if act and act not in deduped_actions:
            deduped_actions.append(act)
    action = " | ".join(deduped_actions)

    return triggered, reasons, action


def _resolve_waveform_path(waveform: str, scenario_dir: Path, data_dir: Path) -> Path:
    candidate = Path(waveform)
    if candidate.is_absolute() and candidate.exists():
        return candidate

    probe_paths = [
        (scenario_dir / candidate).resolve(),
        (scenario_dir.parent / candidate).resolve(),
        (data_dir / candidate).resolve(),
    ]
    for path in probe_paths:
        if path.exists():
            return path
    raise FileNotFoundError(f"Collision waveform not found for '{waveform}' (searched {probe_paths})")


def build_impair_flags(impair: Dict, scenario_dir: Path, data_dir: Path) -> list[str]:
    flags: list[str] = []
    if not impair:
        return flags
    if "cfo_ppm" in impair:
        flags.extend(["--impair-cfo-ppm", str(impair["cfo_ppm"])])
    if "sfo_ppm" in impair:
        flags.extend(["--impair-sfo-ppm", str(impair["sfo_ppm"])])
    if "cfo_drift_ppm" in impair:
        flags.extend(["--impair-cfo-drift-ppm", str(impair["cfo_drift_ppm"])])
    if "sfo_drift_ppm" in impair:
        flags.extend(["--impair-sfo-drift-ppm", str(impair["sfo_drift_ppm"])])
    if "awgn_snr_db" in impair:
        flags.extend(["--impair-awgn-snr", str(impair["awgn_snr_db"])])
    if "burst" in impair:
        burst = impair["burst"]
        flags.extend([
            "--impair-burst-period",
            str(burst.get("period", 0)),
            "--impair-burst-duration",
            str(burst.get("duration", 0)),
            "--impair-burst-snr",
            str(burst.get("snr_db", 0)),
        ])
    if "collision" in impair:
        collision = impair["collision"]
        if collision.get("waveform"):
            waveform_path = _resolve_waveform_path(collision["waveform"], scenario_dir, data_dir)
            flags.extend([
                "--impair-collision-prob",
                str(collision.get("prob", collision.get("probability", 0.0))),
                "--impair-collision-scale",
                str(collision.get("scale", 1.0)),
                "--impair-collision-file",
                str(waveform_path),
            ])
    flags.extend(["--impair-seed", str(impair.get("seed", 0))])
    return flags


def build_rt_flags(rt_cfg: Dict, default_speed: float) -> list[str]:
    if not rt_cfg:
        rt_cfg = {}
    speed = rt_cfg.get("speed", default_speed)
    tolerance = rt_cfg.get("tolerance_ns")
    flags = ["--rt", "--rt-speed", str(speed)]
    if tolerance is not None:
        flags.extend(["--rt-tolerance-ns", str(tolerance)])
    if "max_events" in rt_cfg:
        flags.extend(["--rt-max-events", str(rt_cfg["max_events"])])
    return flags


def run_step(lora_replay: Path, step: Step, summary_dir: Path) -> dict:
    summary_dir.mkdir(parents=True, exist_ok=True)
    summary_path = summary_dir / f"{step.label}_{step.capture.stem}.json"
    cmd = [
        str(lora_replay),
        "--iq",
        str(step.capture),
        "--metadata",
        str(step.metadata),
        "--summary",
        str(summary_path),
    ]
    cmd.extend(["--compare-root", str(step.capture), "--allow-stage-mismatch"])
    cmd.extend(step.impair_flags)
    cmd.extend(step.rt_flags)

    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"lora_replay failed ({result.returncode}): {' '.join(cmd)}")

    data = json.loads(summary_path.read_text())
    data["summary_path"] = str(summary_path)
    return data


def build_steps(manifest_dir: Path, scenario: Dict, default_speed: float, scenario_dir: Path) -> List[Step]:
    steps: List[Step] = []
    for index, step_cfg in enumerate(scenario.get("steps", []), start=1):
        capture = (manifest_dir / step_cfg["capture"]).resolve()

        metadata_cfg = step_cfg.get("metadata")
        if metadata_cfg:
            metadata_path = Path(metadata_cfg)
            if not metadata_path.is_absolute():
                metadata_path = (scenario_dir / metadata_path).resolve()
        else:
            metadata_path = capture.with_suffix(".json")

        impair_flags = build_impair_flags(step_cfg.get("impair", {}), scenario_dir, manifest_dir)
        rt_flags = build_rt_flags(step_cfg.get("rt", {}), default_speed)
        label = f"step{index:02d}"
        expect_fallback = bool(step_cfg.get("expect_fallback", False))
        steps.append(
            Step(
                capture=capture,
                metadata=metadata_path,
                impair_flags=impair_flags,
                rt_flags=rt_flags,
                label=label,
                expect_fallback=expect_fallback,
            )
        )
    return steps


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="JSON array of capture entries (same schema as reference manifest)")
    parser.add_argument("data_dir", type=Path, help="Directory containing capture .cf32/.json pairs")
    parser.add_argument("scenarios", type=Path, help="Scenario definition JSON")
    parser.add_argument("output", type=Path, help="Output JSON summarising the scenarios")
    parser.add_argument("--lora-replay", type=Path, default=Path("host_sim/lora_replay"))
    parser.add_argument("--summary-dir", type=Path, default=Path("build/stage4_adaptive"))
    parser.add_argument(
        "--allow-unexpected-fallbacks",
        action="store_true",
        help="Do not fail when fallback behaviour differs from the scenario expectations",
    )
    args = parser.parse_args()

    lora_replay = args.lora_replay.resolve()
    if not lora_replay.exists():
        raise FileNotFoundError(f"lora_replay not found: {lora_replay}")

    manifest_entries = json.loads(args.manifest.read_text())
    capture_set = {entry["capture"] for entry in manifest_entries}
    scenario_cfg = json.loads(args.scenarios.read_text())
    default_speed = scenario_cfg.get("rt_speed", 1.0)

    results = []
    scenario_dir = args.scenarios.parent
    for scenario in scenario_cfg.get("scenarios", []):
        name = scenario.get("name", "scenario")
        steps = build_steps(args.data_dir, scenario, default_speed, scenario_dir)
        scenario_steps = []
        expected_fallback_steps = 0
        unexpected_fallback_steps = 0
        for step in steps:
            if step.capture.name not in capture_set and not step.metadata.exists():
                raise FileNotFoundError(f"Capture or metadata missing for step: {step.capture}")
            summary = run_step(lora_replay, step, args.summary_dir / name)
            fallback_triggered, fallback_reasons, fallback_action = determine_fallback(summary)
            summary["fallback_triggered"] = fallback_triggered
            if fallback_triggered:
                summary["fallback_reasons"] = fallback_reasons
                if fallback_action:
                    summary["fallback_action"] = fallback_action
            summary["expected_fallback"] = step.expect_fallback
            expected_fallback_steps += int(step.expect_fallback)
            if fallback_triggered != step.expect_fallback:
                unexpected_fallback_steps += 1
                summary["unexpected_fallback"] = True
                if not args.allow_unexpected_fallbacks:
                    raise RuntimeError(
                        f"Fallback expectation mismatch in scenario '{name}' step '{step.label}': "
                        f"expected {step.expect_fallback} got {fallback_triggered}"
                    )
            scenario_steps.append(summary)

        aggregate = {
            "max_tracking_jitter_us": max((s.get("tracking_jitter_us", 0.0) for s in scenario_steps), default=0.0),
            "total_deadline_misses": sum(s.get("deadline_miss_count", 0) for s in scenario_steps),
            "max_packet_error_rate": max((s.get("packet_error_rate", 0.0) for s in scenario_steps), default=0.0),
        }
        aggregate["fallback_steps"] = sum(1 for s in scenario_steps if s.get("fallback_triggered"))
        aggregate["expected_fallback_steps"] = expected_fallback_steps
        aggregate["unexpected_fallback_steps"] = unexpected_fallback_steps
        results.append({
            "name": name,
            "steps": scenario_steps,
            "aggregate": aggregate,
        })

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
