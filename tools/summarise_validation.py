#!/usr/bin/env python3

"""
Aggregate validation outputs (BER, interop, soak) into a Markdown summary.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Optional


def load_json(path: Optional[Path]):
    if not path:
        return None
    if not path.exists():
        raise FileNotFoundError(f"Missing JSON input: {path}")
    return json.loads(path.read_text())


def summarise_ber(data):
    if not data:
        return "No BER data available."
    lines = ["| Capture | SNR (dB) | Avg PER | Avg BER | Max PER | Max BER |", "| --- | ---:| ---:| ---:| ---:| ---:|"]
    for entry in data:
        capture = entry["capture"]
        for point in entry.get("snr_points", []):
            lines.append(
                f"| {capture} | {point['snr_db']:+.1f} | "
                f"{point['avg_packet_error_rate']:.3e} | "
                f"{point['avg_bit_error_rate']:.3e} | "
                f"{point['max_packet_error_rate']:.3e} | "
                f"{point['max_bit_error_rate']:.3e} |"
            )
    return "\n".join(lines)


def summarise_interop(data):
    if not data:
        return "No interop data available."
    lines = ["| Capture | Payload mismatches | CRC mismatch |", "| --- | ---:| --- |"]
    for entry in data:
        mismatch = "yes" if entry.get("crc_mismatch") else "no"
        lines.append(f"| {entry['capture']} | {entry['payload_mismatches']} | {mismatch} |")
    return "\n".join(lines)


def summarise_soak(data):
    if not data:
        return "No soak metrics available."

    def fmt_float(value):
        if value is None:
            return "n/a"
        return f"{value:.1f}"

    lines = [
        "| Mode | Expected | Processed | Produced | Missing | Failures | MTBF (us) | Duration (us) |",
        "| --- | ---:| ---:| ---:| ---:| ---:| ---:| ---:|",
        f"| {data.get('mode', 'unknown')} | {data.get('expected_symbols', 0)} | "
        f"{data.get('processed_symbols', 0)} | {data.get('produced_symbols', 0)} | "
        f"{data.get('missing_symbols', 0)} | {data.get('producer_failures', 0)} | "
        f"{fmt_float(data.get('mtbf_us'))} | {fmt_float(data.get('duration_us'))} |",
    ]

    details = []
    symbol_interval = data.get("symbol_interval_us")
    samples_per_symbol = data.get("samples_per_symbol")
    if symbol_interval or samples_per_symbol:
        details.append(
            f"- Timing: {fmt_float(symbol_interval)} µs per symbol, {samples_per_symbol or 'n/a'} samples/symbol."
        )

    stage_cfg = data.get("stage_config") or {}
    if stage_cfg:
        details.append(
            f"- Stage config: SF{stage_cfg.get('sf', '?')} @ {stage_cfg.get('bandwidth', '?')} Hz BW, "
            f"{stage_cfg.get('sample_rate', '?')} Sa/s."
        )

    capture_info = data.get("capture") or {}
    capture_file = capture_info.get("file")
    if capture_file:
        details.append(
            f"- Capture: `{capture_file}` (metadata: `{capture_info.get('metadata', '')}`)"
        )

    impair = data.get("impairments") or {}
    if impair:
        status = "enabled" if impair.get("enabled") else "disabled"
        parts = [f"- Impairments: {status}"]
        if impair.get("enabled"):
            parts.append(
                f" (seed={impair.get('seed', 0)}, "
                f"CFO={impair.get('cfo_ppm', 0)} ppm, "
                f"SFO={impair.get('sfo_ppm', 0)} ppm, "
                f"AWGN={impair.get('awgn_snr_db', 'n/a')} dB)"
            )
        details.append("".join(parts))

    if details:
        lines.append("")
        lines.extend(details)

    return "\n".join(lines)


def summarise_receiver_compare(data):
    if not data:
        return "No receiver-vs-GNU Radio data available."

    lines = [
        "| Capture | Profile | CFO ppm | STO samples | SFO ppm | GNU Radio (ms) | Standalone (ms) | Status |",
        "| --- | --- | ---:| ---:| ---:| ---:| ---:| --- |",
    ]
    for entry in data:
        impair = entry.get("impairment", {})
        gn = entry.get("gnuradio", {})
        host = entry.get("standalone", {})
        status = f"{gn.get('status', 'n/a')} / {host.get('status', 'n/a')}"
        lines.append(
            f"| {entry.get('capture')} | {entry.get('profile')} | "
            f"{impair.get('cfo_ppm', 0.0):+.1f} | {impair.get('sto_samples', 0)} | {impair.get('sfo_ppm', 0.0):+.1f} | "
            f"{gn.get('wall_s', 0.0) * 1e3:.1f} | {host.get('wall_s', 0.0) * 1e3:.1f} | {status} |"
        )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ber", type=Path, help="Path to stage5_ber_results.json")
    parser.add_argument("--interop", type=Path, help="Path to stage5_interop_results.json")
    parser.add_argument("--soak", type=Path, help="Path to a soak metrics JSON (e.g., build/stage5_live_soak_sf7_metrics.json)")
    parser.add_argument("--receiver-compare", type=Path, help="Path to receiver_vs_gnuradio_results.json")
    parser.add_argument("--output", type=Path, required=True, help="Markdown summary output")
    args = parser.parse_args()

    ber_data = load_json(args.ber)
    interop_data = load_json(args.interop)
    soak_data = load_json(args.soak)
    receiver_compare_data = load_json(args.receiver_compare)

    content = [
        "# Stage 5 Validation Summary",
        "",
        "## BER/PER Sweeps",
        summarise_ber(ber_data),
        "",
        "## Interoperability",
        summarise_interop(interop_data),
        "",
        "## Live Soak Metrics",
        summarise_soak(soak_data),
        "",
        "## Receiver vs GNU Radio",
        summarise_receiver_compare(receiver_compare_data),
        "",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(content))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
