#!/usr/bin/env python3
"""
TX-based impairment sweep — encode with lora_tx, inject CFO/SFO/AWGN,
decode with lora_replay, and measure PER across a parameter grid.

Usage:
    python3 tools/tx_impairment_sweep.py [--matrix matrix.json] [--output-dir build/impairment_sweep]
    python3 tools/tx_impairment_sweep.py --quick   # quick sanity check

The matrix JSON defines the sweep grid.  Example:

    {
      "sf": [7, 9, 12],
      "cr": [1, 4],
      "bw": [125000],
      "snr_db": [null, -5, -8, -10, -12],
      "cfo_hz": [0, 5000, 20000, 40000],
      "sfo_ppm": [0, 5, 20],
      "seeds": 10,
      "payload": "Hello LoRa sweep"
    }

null in snr_db means no noise.  Seeds controls Monte Carlo trials per point.
"""
from __future__ import annotations

import argparse
import itertools
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any, Dict, List, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
LORA_TX = REPO_ROOT / "build" / "host_sim" / "lora_tx"
LORA_REPLAY = REPO_ROOT / "build" / "host_sim" / "lora_replay"

QUICK_MATRIX: Dict[str, Any] = {
    "sf": [7, 12],
    "cr": [1],
    "bw": [125000],
    "snr_db": [None, -8],
    "cfo_hz": [0, 10000],
    "sfo_ppm": [0],
    "seeds": 3,
    "payload": "Hello",
}

DEFAULT_MATRIX: Dict[str, Any] = {
    "sf": [7, 9, 12],
    "cr": [1, 4],
    "bw": [125000],
    "snr_db": [None, -5, -8, -10, -12, -15],
    "cfo_hz": [0, 5000, 20000, 40000],
    "sfo_ppm": [0, 5, 20],
    "seeds": 10,
    "payload": "Hello LoRa sweep",
}


@dataclass
class SweepPoint:
    sf: int
    cr: int
    bw: int
    snr_db: Optional[float]
    cfo_hz: float
    sfo_ppm: float
    seeds: int
    payload: str
    ok_count: int = 0
    per: float = 0.0


def compute_ldro(sf: int, bw: int) -> bool:
    symbol_dur_ms = (2 ** sf) / bw * 1000
    return symbol_dur_ms > 16.0


def run_point(point: SweepPoint, work_dir: Path, lora_tx: Path, lora_replay: Path,
              soft: bool = False) -> SweepPoint:
    """Run one sweep grid point across all seeds."""
    work_dir.mkdir(parents=True, exist_ok=True)

    sample_rate = point.bw * 4
    ldro = compute_ldro(point.sf, point.bw)
    payload_len = len(point.payload)

    meta = {
        "sf": point.sf,
        "bw": point.bw,
        "sample_rate": sample_rate,
        "cr": point.cr,
        "payload_len": payload_len,
        "has_crc": True,
        "implicit_header": False,
        "ldro": ldro,
        "preamble_len": 8,
        "sync_word": 18,
    }
    meta_path = work_dir / "meta.json"
    meta_path.write_text(json.dumps(meta))

    ok = 0
    for seed in range(1, point.seeds + 1):
        iq_path = work_dir / f"seed{seed}.cf32"

        # Build TX command
        tx_cmd = [
            str(lora_tx),
            "--sf", str(point.sf),
            "--cr", str(point.cr),
            "--bw", str(point.bw),
            "--sample-rate", str(sample_rate),
            "--payload", point.payload,
            "--output", str(iq_path),
        ]
        if point.snr_db is not None:
            tx_cmd.extend(["--snr", str(point.snr_db), "--seed", str(seed)])
        if point.cfo_hz != 0:
            tx_cmd.extend(["--cfo", str(point.cfo_hz)])
        if point.sfo_ppm != 0:
            tx_cmd.extend(["--sfo", str(point.sfo_ppm)])

        try:
            subprocess.run(tx_cmd, check=True, capture_output=True, timeout=30)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
            print(f"  TX failed: {e}", file=sys.stderr)
            continue

        # Decode
        rx_cmd = [
            str(lora_replay),
            "--iq", str(iq_path),
            "--metadata", str(meta_path),
            "--payload", point.payload,
        ]
        if soft:
            rx_cmd.append("--soft")

        try:
            result = subprocess.run(rx_cmd, capture_output=True, timeout=60)
            stdout = result.stdout.decode("utf-8", errors="replace")
        except (subprocess.TimeoutExpired, Exception):
            stdout = ""

        if re.search(r"\[payload\] CRC.*?OK", stdout):
            ok += 1

        # Clean up IQ file to save disk
        iq_path.unlink(missing_ok=True)

    point.ok_count = ok
    point.per = 1.0 - ok / point.seeds if point.seeds > 0 else 1.0
    return point


def run_sweep(matrix: Dict[str, Any], output_dir: Path,
              lora_tx: Path, lora_replay: Path, soft: bool = False) -> List[SweepPoint]:
    """Run the full sweep grid."""
    results: List[SweepPoint] = []
    payload = matrix.get("payload", "Hello")
    seeds = matrix.get("seeds", 5)

    grid = list(itertools.product(
        matrix.get("sf", [7]),
        matrix.get("cr", [1]),
        matrix.get("bw", [125000]),
        matrix.get("snr_db", [None]),
        matrix.get("cfo_hz", [0]),
        matrix.get("sfo_ppm", [0]),
    ))

    total = len(grid)
    print(f"Sweep: {total} grid points x {seeds} seeds = {total * seeds} trials")

    for i, (sf, cr, bw, snr, cfo, sfo) in enumerate(grid, 1):
        snr_str = f"SNR={snr}dB" if snr is not None else "no-noise"
        label = f"SF{sf} CR{cr} BW{bw//1000}k {snr_str} CFO={cfo}Hz SFO={sfo}ppm"
        print(f"[{i}/{total}] {label} ...", end=" ", flush=True)

        point = SweepPoint(
            sf=sf, cr=cr, bw=bw, snr_db=snr,
            cfo_hz=cfo, sfo_ppm=sfo, seeds=seeds, payload=payload,
        )
        work = output_dir / f"sf{sf}_cr{cr}_bw{bw}_snr{snr}_cfo{cfo}_sfo{sfo}"
        run_point(point, work, lora_tx=lora_tx, lora_replay=lora_replay, soft=soft)

        status = "PASS" if point.per == 0 else ("PARTIAL" if point.per < 1.0 else "FAIL")
        print(f"{point.ok_count}/{seeds} OK (PER={point.per:.0%}) [{status}]")
        results.append(point)

    return results


def write_report(results: List[SweepPoint], output_dir: Path) -> None:
    """Write JSON and markdown summary."""
    output_dir.mkdir(parents=True, exist_ok=True)

    # JSON
    json_path = output_dir / "sweep_results.json"
    json_data = []
    for r in results:
        json_data.append({
            "sf": r.sf, "cr": r.cr, "bw": r.bw,
            "snr_db": r.snr_db, "cfo_hz": r.cfo_hz, "sfo_ppm": r.sfo_ppm,
            "seeds": r.seeds, "ok_count": r.ok_count, "per": r.per,
        })
    json_path.write_text(json.dumps(json_data, indent=2) + "\n")

    # Markdown
    md_path = output_dir / "sweep_summary.md"
    lines = [
        "# TX Impairment Sweep Results\n",
        "Controlled impairment injection via `lora_tx` → decode via `lora_replay`.\n",
        "| SF | CR | BW | SNR (dB) | CFO (Hz) | SFO (ppm) | OK/Total | PER |",
        "|----|----|-----|----------|----------|-----------|----------|-----|",
    ]
    for r in results:
        snr_str = f"{r.snr_db}" if r.snr_db is not None else "∞"
        status = "pass" if r.per == 0 else ("partial" if r.per < 1.0 else "**FAIL**")
        lines.append(
            f"| {r.sf} | {r.cr} | {r.bw//1000}k | {snr_str} "
            f"| {r.cfo_hz} | {r.sfo_ppm} "
            f"| {r.ok_count}/{r.seeds} | {r.per:.0%} {status} |"
        )

    # Summary stats
    total_points = len(results)
    pass_points = sum(1 for r in results if r.per == 0)
    partial_points = sum(1 for r in results if 0 < r.per < 1.0)
    fail_points = sum(1 for r in results if r.per == 1.0)
    lines.append(f"\n**Summary:** {pass_points}/{total_points} PASS, "
                 f"{partial_points} partial, {fail_points} FAIL\n")

    md_path.write_text("\n".join(lines) + "\n")
    print(f"\nReports: {json_path}\n         {md_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--matrix", type=Path, help="Sweep matrix JSON")
    parser.add_argument("--output-dir", type=Path,
                        default=REPO_ROOT / "build" / "impairment_sweep")
    parser.add_argument("--quick", action="store_true",
                        help="Run a quick sanity check with minimal grid")
    parser.add_argument("--soft", action="store_true",
                        help="Enable soft-decision decoding")
    parser.add_argument("--lora-tx", type=Path, default=LORA_TX)
    parser.add_argument("--lora-replay", type=Path, default=LORA_REPLAY)
    args = parser.parse_args()

    lora_tx = args.lora_tx.resolve()
    lora_replay = args.lora_replay.resolve()

    if not lora_tx.exists():
        print(f"lora_tx not found: {lora_tx}", file=sys.stderr)
        return 1
    if not lora_replay.exists():
        print(f"lora_replay not found: {lora_replay}", file=sys.stderr)
        return 1

    if args.quick:
        matrix = QUICK_MATRIX
    elif args.matrix:
        matrix = json.loads(args.matrix.read_text())
    else:
        matrix = DEFAULT_MATRIX

    output_dir = args.output_dir
    results = run_sweep(matrix, output_dir, lora_tx=lora_tx,
                        lora_replay=lora_replay, soft=args.soft)
    write_report(results, output_dir)

    # Exit code 1 if any point has 100% PER at moderate SNR (not expected to fail)
    unexpected_fails = [
        r for r in results
        if r.per == 1.0 and (r.snr_db is None or r.snr_db >= -8)
        and r.cfo_hz <= 40000 and r.sfo_ppm <= 20
    ]
    if unexpected_fails:
        print(f"\n{len(unexpected_fails)} unexpected failures!")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
