#!/usr/bin/env python3

"""
Run BER/PER sweeps by varying SNR and seeds, aggregating results into JSON (and optional CSV).
"""

from __future__ import annotations

import argparse
import csv
import json
import statistics
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


@dataclass
class SweepRun:
    capture: Path
    metadata: Path
    snr_db: float
    seed: int
    summary_path: Path

    def summary_filename(self) -> Path:
        capture_name = self.capture.stem
        snr_part = f"snr{self.snr_db:+.1f}".replace(".", "p").replace("+", "p").replace("-", "m")
        return self.summary_path / f"{capture_name}_{snr_part}_seed{self.seed}.json"


def load_manifest(manifest_path: Path) -> List[Dict[str, Any]]:
    return json.loads(manifest_path.read_text())


def run_lora_replay(
    lora_replay: Path,
    run: SweepRun,
    impair_flags: List[str],
    instrument_mode: str,
    extra_args: Optional[List[str]] = None,
) -> Dict[str, Any]:
    summary_file = run.summary_filename()
    summary_file.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(lora_replay),
        "--iq",
        str(run.capture),
        "--metadata",
        str(run.metadata),
        "--summary",
        str(summary_file),
        "--instrument-mode",
        instrument_mode,
    ]
    if extra_args:
        cmd.extend(extra_args)
    cmd.extend(impair_flags)
    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"lora_replay failed ({result.returncode}): {' '.join(cmd)}")

    summary = json.loads(summary_file.read_text())
    summary["summary_path"] = str(summary_file)
    return summary


def build_impair_flags(base_impair: Dict[str, Any], snr_db: float, seed: int) -> List[str]:
    flags: List[str] = []

    if "cfo_ppm" in base_impair:
        flags.extend(["--impair-cfo-ppm", str(base_impair["cfo_ppm"])])
    if "sfo_ppm" in base_impair:
        flags.extend(["--impair-sfo-ppm", str(base_impair["sfo_ppm"])])
    if "cfo_drift_ppm" in base_impair:
        flags.extend(["--impair-cfo-drift-ppm", str(base_impair["cfo_drift_ppm"])])
    if "sfo_drift_ppm" in base_impair:
        flags.extend(["--impair-sfo-drift-ppm", str(base_impair["sfo_drift_ppm"])])
    if "burst" in base_impair:
        burst = base_impair["burst"]
        flags.extend(
            [
                "--impair-burst-period",
                str(burst.get("period", 0)),
                "--impair-burst-duration",
                str(burst.get("duration", 0)),
                "--impair-burst-snr",
                str(burst.get("snr_db", 0)),
            ]
        )
    if "collision" in base_impair:
        collision = base_impair["collision"]
        flags.extend(
            [
                "--impair-collision-prob",
                str(collision.get("prob", collision.get("probability", 0.0))),
                "--impair-collision-scale",
                str(collision.get("scale", 1.0)),
            ]
        )
        if collision.get("waveform"):
            flags.extend(["--impair-collision-file", collision["waveform"]])

    flags.extend(["--impair-awgn-snr", str(snr_db)])
    flags.extend(["--impair-seed", str(seed)])
    return flags


def aggregate_runs(runs: List[Dict[str, Any]]) -> Dict[str, Any]:
    if not runs:
        return {
            "avg_packet_error_rate": 0.0,
            "avg_bit_error_rate": 0.0,
            "max_packet_error_rate": 0.0,
            "max_bit_error_rate": 0.0,
            "reference_mismatches": 0.0,
        }

    per = [run.get("packet_error_rate", 0.0) for run in runs]
    ber = [run.get("bit_error_rate", 0.0) for run in runs]
    mismatches = [run.get("reference_mismatches", 0.0) for run in runs]

    return {
        "avg_packet_error_rate": statistics.fmean(per),
        "avg_bit_error_rate": statistics.fmean(ber),
        "max_packet_error_rate": max(per),
        "max_bit_error_rate": max(ber),
        "avg_reference_mismatches": statistics.fmean(mismatches),
        "max_reference_mismatches": max(mismatches),
    }


def write_csv(csv_path: Path, records: List[Dict[str, Any]]) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "capture",
        "snr_db",
        "seed",
        "packet_error_rate",
        "bit_error_rate",
        "reference_mismatches",
        "summary_path",
    ]
    with csv_path.open("w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        writer.writeheader()
        for record in records:
            writer.writerow(record)


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("data_dir", type=Path)
    parser.add_argument("output", type=Path, help="Aggregated JSON output")
    parser.add_argument("--matrix", type=Path, required=True, help="Sweep definition JSON")
    parser.add_argument("--summary-dir", type=Path, default=Path("build/stage5_ber_summaries"))
    parser.add_argument("--lora-replay", type=Path, default=Path("host_sim/lora_replay"))
    parser.add_argument("--instrument-mode", choices=["float", "q15"], default="float")
    parser.add_argument("--extra-args", nargs=argparse.REMAINDER, default=[])
    parser.add_argument("--csv", type=Path, help="Optional CSV output path")
    args = parser.parse_args(argv)

    lora_replay = args.lora_replay.resolve()
    if not lora_replay.exists():
        raise FileNotFoundError(f"lora_replay not found: {lora_replay}")

    manifest = load_manifest(args.manifest)
    matrix = json.loads(args.matrix.read_text())

    captures_filter = {Path(name).name for name in matrix.get("captures", [])}
    snr_values = matrix.get("snr_db", [])
    if not snr_values:
        raise ValueError("matrix JSON must include non-empty 'snr_db' list")
    seeds = matrix.get("seeds", [0])
    base_impair = matrix.get("impairments", {})

    summary_dir = args.summary_dir.resolve()
    all_records: List[Dict[str, Any]] = []
    aggregated: List[Dict[str, Any]] = []

    for entry in manifest:
        capture_name = Path(entry["capture"]).name
        if captures_filter and capture_name not in captures_filter:
            continue

        capture_path = args.data_dir / entry["capture"]
        metadata_path = capture_path.with_suffix(".json")

        capture_entry = {
            "capture": entry["capture"],
            "snr_points": [],
        }

        for snr in snr_values:
            run_records: List[Dict[str, Any]] = []
            for seed in seeds:
                run = SweepRun(
                    capture=capture_path,
                    metadata=metadata_path,
                    snr_db=snr,
                    seed=seed,
                    summary_path=summary_dir / capture_name,
                )
                flags = build_impair_flags(base_impair, snr, seed)
                summary = run_lora_replay(lora_replay, run, flags, args.instrument_mode, args.extra_args)

                record = {
                    "capture": entry["capture"],
                    "snr_db": snr,
                    "seed": seed,
                    "packet_error_rate": summary.get("packet_error_rate", 0.0),
                    "bit_error_rate": summary.get("bit_error_rate", 0.0),
                    "reference_mismatches": summary.get("reference_mismatches", 0),
                    "summary_path": summary.get("summary_path"),
                }
                run_records.append(record)
                all_records.append(record)

            aggregates = aggregate_runs(run_records)
            capture_entry["snr_points"].append(
                {
                    "snr_db": snr,
                    "runs": run_records,
                    **aggregates,
                }
            )

        aggregated.append(capture_entry)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(aggregated, indent=2) + "\n")

    if args.csv and all_records:
        write_csv(args.csv, all_records)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
