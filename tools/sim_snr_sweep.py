#!/usr/bin/env python3
"""
Software SNR sensitivity sweep — inject noise into real OTA captures and
measure decode success rate at various SNR levels.

Usage:
    python3 tools/sim_snr_sweep.py [--output-dir build/snr_sweep]
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

import numpy as np

REPO_ROOT = Path(__file__).resolve().parent.parent
LORA_REPLAY = REPO_ROOT / "build" / "host_sim" / "lora_replay"


@dataclass
class CaptureInfo:
    name: str
    cf32_path: Path
    meta_path: Path
    sf: int
    bw: int


@dataclass
class SnrResult:
    capture: str
    sf: int
    snr_db: float
    crc_ok: bool
    payload: str
    error: str
    pass_count: int = 0
    total_count: int = 0


def load_iq(path: Path) -> np.ndarray:
    """Load interleaved float32 IQ as complex64 array."""
    raw = np.fromfile(str(path), dtype=np.float32)
    return raw.view(np.complex64)


def save_iq(data: np.ndarray, path: Path) -> None:
    """Save complex64 array as interleaved float32."""
    data.astype(np.complex64).view(np.float32).tofile(str(path))


def add_awgn(signal: np.ndarray, snr_db: float) -> np.ndarray:
    """Add AWGN to achieve target SNR (dB) relative to signal power."""
    sig_power = np.mean(np.abs(signal) ** 2)
    if sig_power == 0:
        return signal

    # Target noise power for desired SNR
    snr_linear = 10 ** (snr_db / 10)
    noise_power = sig_power / snr_linear

    noise = np.sqrt(noise_power / 2) * (
        np.random.randn(len(signal)) + 1j * np.random.randn(len(signal))
    )
    return signal + noise.astype(np.complex64)


def decode_capture(cf32_path: Path, meta_path: Path) -> tuple[bool, str]:
    """Decode and return (crc_ok, payload_ascii)."""
    cmd = [str(LORA_REPLAY), "--iq", str(cf32_path), "--metadata", str(meta_path)]
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=600)
        stdout = result.stdout.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired:
        return (False, "")
    except Exception:
        return (False, "")

    crc_ok = bool(re.search(r"\[payload\] CRC.*?OK", stdout))
    payloads = re.findall(r"Payload ASCII:\s*(.+)", stdout)
    payload = payloads[0].strip() if payloads else ""
    return (crc_ok, payload)


def discover_golden_captures() -> List[CaptureInfo]:
    """Find golden OTA captures in host_sim/data/ota/."""
    ota_dir = REPO_ROOT / "host_sim" / "data" / "ota"
    captures = []

    for cf32 in sorted(ota_dir.glob("*.cf32")):
        # Find matching metadata
        meta = cf32.with_name(cf32.stem + "_meta.json")
        if not meta.exists():
            # Try without suffix
            meta = cf32.with_suffix(".json")
            if not meta.exists():
                continue

        try:
            md = json.loads(meta.read_text())
            sf = md.get("sf", 0)
            bw = md.get("bw", 0)
            captures.append(CaptureInfo(
                name=cf32.stem,
                cf32_path=cf32,
                meta_path=meta,
                sf=sf,
                bw=bw,
            ))
        except Exception:
            continue

    return captures


def run_snr_sweep(
    captures: List[CaptureInfo],
    snr_range: List[float],
    output_dir: Path,
    trials: int = 5,
) -> List[SnrResult]:
    """Run SNR sweep on each capture at each SNR level."""
    results: List[SnrResult] = []
    output_dir.mkdir(parents=True, exist_ok=True)

    for cap in captures:
        print(f"\n{'='*60}")
        print(f"Capture: {cap.name} (SF{cap.sf}, BW{cap.bw//1000}k)")
        print(f"{'='*60}")

        # Load original IQ
        original = load_iq(cap.cf32_path)

        # Estimate signal-only power from the burst region
        # (Use the existing capture as-is — it contains signal + noise from HackRF)
        sig_power_db = 10 * np.log10(np.mean(np.abs(original) ** 2) + 1e-30)
        print(f"  Signal power: {sig_power_db:.1f} dB")

        for snr_db in snr_range:
            successes = 0
            last_payload = ""

            for trial in range(trials):
                # Add noise
                noisy = add_awgn(original, snr_db)

                # Write to temp file
                with tempfile.NamedTemporaryFile(suffix=".cf32", delete=False) as f:
                    tmp_path = Path(f.name)
                    save_iq(noisy, tmp_path)

                try:
                    crc_ok, payload = decode_capture(tmp_path, cap.meta_path)
                    if crc_ok:
                        successes += 1
                        last_payload = payload
                finally:
                    tmp_path.unlink(missing_ok=True)

            per = 1.0 - successes / trials
            status = "✅" if successes > 0 else "❌"
            print(f"  SNR={snr_db:+6.1f} dB: {successes}/{trials} OK "
                  f"(PER={per:.0%}) {status}"
                  + (f" [{last_payload}]" if last_payload else ""))

            results.append(SnrResult(
                capture=cap.name,
                sf=cap.sf,
                snr_db=snr_db,
                crc_ok=successes > 0,
                payload=last_payload if successes > 0 else "",
                error="" if successes > 0 else f"PER={per:.0%}",
                pass_count=successes,
                total_count=trials,
            ))

    return results


def write_report(results: List[SnrResult], output_dir: Path) -> None:
    """Write JSON results and markdown summary."""
    # JSON
    json_path = output_dir / "snr_sweep_results.json"
    json_path.write_text(json.dumps(
        [r.__dict__ for r in results], indent=2
    ) + "\n")

    # Markdown summary
    md_path = output_dir / "snr_sweep_summary.md"
    captures = sorted(set(r.capture for r in results))
    snrs = sorted(set(r.snr_db for r in results))

    lines = ["# SNR Sensitivity Sweep Results\n"]
    lines.append("Software noise injection into real OTA captures.\n")

    for cap in captures:
        cap_results = [r for r in results if r.capture == cap]
        sf = cap_results[0].sf if cap_results else "?"
        bw = "?"
        # Try to get BW from capture name
        m = re.search(r"bw(\d+)", cap)
        if m:
            bw = m.group(1)
        lines.append(f"\n## {cap} (SF{sf}, BW{bw}k)\n")
        lines.append("| SNR (dB) | Pass/Total | PER | Payload |")
        lines.append("|----------|-----------|-----|---------|")
        for r in sorted(cap_results, key=lambda x: -x.snr_db):
            per = 1.0 - r.pass_count / r.total_count if r.total_count > 0 else 1.0
            status = "✅" if r.pass_count == r.total_count else ("⚠️" if r.pass_count > 0 else "❌")
            lines.append(
                f"| {r.snr_db:+.1f} | {r.pass_count}/{r.total_count} | "
                f"{per:.0%} {status} | {r.payload} |"
            )

    md_path.write_text("\n".join(lines) + "\n")
    print(f"\nReports written to:\n  {json_path}\n  {md_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", default=str(REPO_ROOT / "build" / "snr_sweep"),
                        help="Output directory")
    parser.add_argument("--snr-min", type=float, default=-15.0,
                        help="Minimum SNR (dB)")
    parser.add_argument("--snr-max", type=float, default=20.0,
                        help="Maximum SNR (dB)")
    parser.add_argument("--snr-step", type=float, default=5.0,
                        help="SNR step (dB)")
    parser.add_argument("--trials", type=int, default=5,
                        help="Trials per SNR level")
    parser.add_argument("--sf", type=int, nargs="*",
                        help="Only test specific SFs")
    args = parser.parse_args()

    output_dir = Path(args.output_dir)

    # Build SNR range
    snr_range = []
    snr = args.snr_max
    while snr >= args.snr_min:
        snr_range.append(snr)
        snr -= args.snr_step

    captures = discover_golden_captures()
    if args.sf:
        captures = [c for c in captures if c.sf in args.sf]

    if not captures:
        print("No golden captures found!", file=sys.stderr)
        return 1

    # Select representative captures (one per SF)
    seen_sf = set()
    representative = []
    for c in captures:
        if c.sf not in seen_sf:
            seen_sf.add(c.sf)
            representative.append(c)
    captures = sorted(representative, key=lambda c: c.sf)

    print(f"SNR sweep: {len(captures)} captures, "
          f"SNR range [{args.snr_min}, {args.snr_max}] dB, "
          f"step={args.snr_step} dB, "
          f"trials={args.trials}")
    for c in captures:
        print(f"  - {c.name} (SF{c.sf}, BW{c.bw//1000}k)")

    results = run_snr_sweep(captures, snr_range, output_dir, args.trials)
    write_report(results, output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
