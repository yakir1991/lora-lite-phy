#!/usr/bin/env python3
"""Regenerate docs/receiver_vs_gnuradio_summary.md from results JSON."""

from __future__ import annotations

import json
from pathlib import Path
from statistics import median
from typing import Dict, List, Any


def load_matrix_metadata(matrix_path: Path) -> Dict[str, Dict[str, Any]]:
    matrix = json.loads(matrix_path.read_text())
    repo_root = matrix_path.resolve().parents[1]
    lookup: Dict[str, Dict[str, Any]] = {}
    for entry in matrix.get("captures", []):
        meta_path = Path(entry["metadata"])
        if not meta_path.is_absolute():
            meta_path = (repo_root / meta_path).resolve()
        meta = json.loads(meta_path.read_text())
        override = entry.get("override") or {}
        meta.update(override)
        lookup[entry["name"]] = meta
    return lookup


def median_or_zero(values: List[float]) -> float:
    return median(values) if values else 0.0


def shorten_hex(value: str | None, limit: int = 32) -> str:
    if not value:
        return "-"
    return value if len(value) <= limit else value[:limit] + "…"


def decode_status(status: str, payload_hex: str | None) -> str:
    if status == "ok" and payload_hex:
        return "ok"
    if status == "ok":
        return "no-payload"
    return status


def build_summary(results_path: Path, matrix_path: Path, output_path: Path) -> None:
    results = json.loads(results_path.read_text())
    meta_lookup = load_matrix_metadata(matrix_path)

    rows: List[Dict[str, Any]] = []
    stage_summary: Dict[str, Dict[str, List[float]]] = {}
    stage_rows = 0

    for item in results:
        name = item["capture"]
        meta = meta_lookup.get(name)
        if not meta:
            continue
        bw_hz = float(meta.get("bw") or meta.get("bandwidth_hz") or 0)
        bw_khz = bw_hz / 1000.0 if bw_hz else 0.0
        cr = meta.get("cr") or meta.get("cr_index") or 0
        crc = meta.get("has_crc")
        if crc is None:
            crc = meta.get("crc_enabled", True)
        implicit = bool(meta.get("implicit_header", False))
        gn_metrics = item["gnuradio"]
        host_metrics = item["standalone"]
        payload = item.get("payload") or {}
        rows.append(
            {
                "capture": name,
                "profile": item["profile"],
                "mc_iteration": item.get("mc_iteration", 0),
                "sf": int(meta.get("sf", 0)),
                "bw_khz": bw_khz,
                "cr": cr,
                "crc": "yes" if crc else "no",
                "implicit": "yes" if implicit else "no",
                "gn_ms": gn_metrics["wall_s"] * 1000.0,
                "host_ms": host_metrics["wall_s"] * 1000.0,
                "speedup": (gn_metrics["wall_s"] / host_metrics["wall_s"]) if host_metrics["wall_s"] > 0 else float("inf"),
                "gn_ram": gn_metrics.get("resources", {}).get("max_rss_kb"),
                "host_ram": host_metrics.get("resources", {}).get("max_rss_kb"),
                "gn_cpu": gn_metrics.get("resources", {}).get("cpu_percent"),
                "host_cpu": host_metrics.get("resources", {}).get("cpu_percent"),
                "expected_payload_hex": payload.get("expected_hex"),
                "standalone_payload_hex": payload.get("standalone_hex"),
                "gn_status": decode_status(gn_metrics["status"], payload.get("expected_hex")),
                "host_status": decode_status(host_metrics["status"], payload.get("standalone_hex")),
            }
        )
        stage_info = host_metrics.get("summary", {}).get("stage_instrumentation") or []
        if stage_info:
            stage_rows += 1
        for entry in stage_info:
            label = entry.get("label") or f"stage_{entry.get('index', 0)}"
            bucket = stage_summary.setdefault(label, {"avg_ns": [], "max_ns": [], "max_scratch": []})
            if entry.get("avg_ns") is not None:
                bucket["avg_ns"].append(entry["avg_ns"])
            if entry.get("max_ns") is not None:
                bucket["max_ns"].append(entry["max_ns"])
            if entry.get("max_scratch_bytes") is not None:
                bucket["max_scratch"].append(entry["max_scratch_bytes"])

    rows.sort(key=lambda r: (r["capture"], r["profile"], r["mc_iteration"]))
    if not rows:
        raise SystemExit("No comparison rows found")

    unique_caps = sorted({r["capture"] for r in rows})
    unique_profiles = sorted({r["profile"] for r in rows})
    unique_sf = sorted({r["sf"] for r in rows})
    unique_bw = sorted({r["bw_khz"] for r in rows})
    speedups = [r["speedup"] for r in rows if r["speedup"] not in (None, float("inf"))]
    host_faster = sum(1 for s in speedups if s > 1.0)
    host_slower = len(rows) - host_faster
    best = max(rows, key=lambda r: r["speedup"])
    worst = min(rows, key=lambda r: r["speedup"])

    ram_gn = [r["gn_ram"] for r in rows if isinstance(r["gn_ram"], (int, float))]
    ram_host = [r["host_ram"] for r in rows if isinstance(r["host_ram"], (int, float))]
    cpu_gn = [r["gn_cpu"] for r in rows if isinstance(r["gn_cpu"], (int, float))]
    cpu_host = [r["host_cpu"] for r in rows if isinstance(r["host_cpu"], (int, float))]

    stage_lines = []
    for label, data in sorted(stage_summary.items()):
        stage_lines.append(
            f"| {label} | {len(data['avg_ns'])} | {median_or_zero(data['avg_ns']):,.0f} | "
            f"{(max(data['max_ns']) if data['max_ns'] else 0):,.0f} | "
            f"{(max(data['max_scratch']) if data['max_scratch'] else 0):,} |"
        )

    lines: List[str] = []
    lines.append("# GNU Radio vs Standalone – Expanded Comparison")
    lines.append("")
    lines.append("Latency, memory, and CPU metrics come from `/usr/bin/time -v` around each decoder run.")
    lines.append("")
    lines.append(
        f"**{len(rows)}** capture/profile/MC tuples over **{len(unique_caps)}** capture configs and **{len(unique_profiles)}** impairment profiles. "
        f"SF range {min(unique_sf)}–{max(unique_sf)}, BW {min(unique_bw):.1f}–{max(unique_bw):.1f} kHz. "
        f"Host faster on {host_faster}/{len(rows)} cases (max speedup {best['speedup']:.1f}× at {best['capture']} / {best['profile']} / mc{best['mc_iteration']}), "
        f"slower on {host_slower} (worst {worst['speedup']:.1f}× at {worst['capture']} / {worst['profile']} / mc{worst['mc_iteration']})."
    )
    lines.append("")
    lines.append(
        f"Median GNU Radio RSS: {median_or_zero(ram_gn):,.0f} kB vs. standalone {median_or_zero(ram_host):,.0f} kB; "
        f"median CPU load {median_or_zero(cpu_gn):.0f}% vs. {median_or_zero(cpu_host):.0f}% (includes MC iterations)."
    )
    lines.append("")
    lines.append("## Synthetic Coverage Highlights (No-HW)")
    lines.append("- Captures span implicit/explicit headers, CRC on/off, payloads 8–256 bytes, BW 62.5–500 kHz, and SF5–SF12.")
    lines.append("- Impairment profiles now include static CFO/STO/SFO, burst noise, collision overlays, drifting CFO/SFO, and IQ-imbalance stress for the SF10@500 kHz case.")
    lines.append("- CI helper `tools/run_receiver_vs_gnuradio_ci.sh --batch <mid|low|high> --merge` runs each batch with `MC_RUNS=2`, doubling coverage for low-SNR/burst cases while staying deterministic.")
    lines.append(
        f"- Per-stage instrumentation is emitted by `lora_replay` for {stage_rows}/{len(rows)} runs (~{(stage_rows/len(rows))*100:.1f}%). "
        "Use the table below to spot slow stages before OTA hardware arrives."
    )
    lines.append("- Each row now reports whether GNU Radio and the standalone decoder produced payload bytes, alongside the hex dumps themselves.")
    lines.append("")
    lines.append("## Figures")
    lines.append("![Runtime](images/receiver_compare_runtime.png)")
    lines.append("![Speedup](images/receiver_compare_speedup.png)")
    lines.append("![Coverage](images/receiver_compare_config.png)")
    lines.append("![RAM](images/receiver_compare_ram.png)")
    lines.append("![CPU](images/receiver_compare_cpu.png)")
    lines.append("")
    lines.append("## Stage-Level Instrumentation Summary")
    if stage_lines:
        lines.append("| Stage | Samples | Median avg ns | Max ns | Max scratch bytes |")
        lines.append("| --- | ---:| ---:| ---:| ---:|")
        lines.extend(stage_lines)
    else:
        lines.append("_Stage timings pending next instrumentation-enabled run._")
    lines.append("")
    lines.append("## Detailed Results (per MC iteration)")
    lines.append("")
    lines.append(
        "| Capture | Profile | MC | SF | BW (kHz) | CR | CRC | ImplHdr | GNU ms | Standalone ms | Speedup | "
        "GNU RAM (kB) | Standalone RAM (kB) | GNU CPU (%) | Standalone CPU (%) | Expected Payload (hex) | "
        "Standalone Payload (hex) | GNU Decode | Standalone Decode |"
    )
    lines.append("| --- | --- | ---:| ---:| ---:| ---:| --- | --- | ---:| ---:| ---:| ---:| ---:| ---:| ---:| --- | --- | --- | --- |")
    for r in rows:
        lines.append(
            f"| {r['capture']} | {r['profile']} | {r['mc_iteration']} | {r['sf']} | {r['bw_khz']:.1f} | {r['cr']} | {r['crc']} | {r['implicit']} | "
            f"{r['gn_ms']:.1f} | {r['host_ms']:.1f} | {r['speedup']:.1f}× | {r['gn_ram'] if r['gn_ram'] is not None else '-'} | "
            f"{r['host_ram'] if r['host_ram'] is not None else '-'} | {r['gn_cpu'] if r['gn_cpu'] is not None else '-'} | "
            f"{r['host_cpu'] if r['host_cpu'] is not None else '-'} | {shorten_hex(r['expected_payload_hex'])} | "
            f"{shorten_hex(r['standalone_payload_hex'])} | {r['gn_status']} | {r['host_status']} |"
        )

    output_path.write_text("\n".join(lines) + "\n")


def main() -> None:
    repo = Path(__file__).resolve().parents[1]
    results = repo / "docs/receiver_vs_gnuradio_results.json"
    matrix = repo / "docs/receiver_vs_gnuradio_matrix.json"
    output = repo / "docs/receiver_vs_gnuradio_summary.md"
    build_summary(results, matrix, output)


if __name__ == "__main__":
    main()
