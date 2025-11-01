#!/usr/bin/env python3
"""
Automated channel regression harness for the LoRa Lite PHY repository.

Before running, ensure the GNU Radio environment is active (e.g.
``conda activate gr310``) so that vector generation scripts can import the
required modules.

This helper script regenerates a set of impaired LoRa IQ vectors, validates the
C++ receiver (`decode_cli`) as well as the GNU Radio vs. streaming comparison
(`tools.compare_streaming_compat`), and produces a single JSON summary that can
be consumed by CI.

The coverage includes:
  * CFO sweep (0 → 200 Hz)
  * Rayleigh fading sweep (Doppler 0.5 → 3 Hz)
  * Sampling offset + ADC quantisation combinations
  * CFO + drift + sampling offset grid
  * Multipath fading severity sweep
  * Five bespoke “new case” stress scenarios

Outputs land under:
  vectors/{...}/          Generated clean/impaired vectors + metadata
  results/channel_regression_summary.json   Aggregate decode/streaming report

Example:
    python -m tools.run_channel_regressions --regen
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Tuple

REPO_ROOT = Path(__file__).resolve().parents[1]
EXPORT_SCRIPT = REPO_ROOT / "external/gr_lora_sdr/scripts/export_tx_reference_vector.py"
COMPARE_SCRIPT = "tools.compare_streaming_compat"
STREAMING_REPORT_NAME = "streaming_report.json"
FILENAME = "tx_sf8_bw125000_cr4_crc1_impl0_ldro2_pay17.cf32"

DECODE_CANDIDATES = [
    REPO_ROOT / "cpp_receiver/build/decode_cli",
    REPO_ROOT / "cpp_receiver/build/Release/decode_cli",
    REPO_ROOT / "cpp_receiver/build/Debug/decode_cli",
]


def resolve_decode_cli() -> Path:
    for candidate in DECODE_CANDIDATES:
        if candidate.exists():
            return candidate
    raise FileNotFoundError("decode_cli not found; build cpp_receiver first")


def run_subprocess(cmd: List[str], **kwargs) -> subprocess.CompletedProcess:
    kwargs.setdefault("capture_output", True)
    kwargs.setdefault("text", True)
    kwargs.setdefault("errors", "replace")
    return subprocess.run(cmd, **kwargs)


def build_export_args(
    sf: int,
    bw: int,
    samp_rate: int,
    payload: str,
    cr: int,
    clean_dir: Path,
    air_dir: Path,
    channel_kwargs: Dict[str, object],
) -> List[str]:
    flag_map = {
        "cfo_hz": "--cfo-hz",
        "cfo_drift_hz": "--cfo-drift-hz",
        "sampling_offset_ppm": "--sampling-offset-ppm",
        "phase_noise_lw": "--phase-noise-lw",
        "multipath_taps": "--multipath-taps",
        "snr_db": "--snr-db",
        "quantize_bits": "--quantize-bits",
        "clip": "--clip",
        "fading_doppler_hz": "--fading-doppler-hz",
    }

    args = [
        sys.executable,
        str(EXPORT_SCRIPT),
        "--emit-frame",
        "--sf",
        str(sf),
        "--bw",
        str(bw),
        "--samp-rate",
        str(samp_rate),
        "--payload",
        payload,
        "--cr",
        str(cr),
        "--channel-sim",
        "--out-dir",
        str(clean_dir),
        "--air-output",
        str(air_dir),
    ]

    for key, flag in flag_map.items():
        if key in channel_kwargs and channel_kwargs[key] is not None:
            args.extend([flag, str(channel_kwargs[key])])
    if channel_kwargs.get("rayleigh_fading"):
        args.append("--rayleigh-fading")

    return args


def ensure_vector(
    sf: int,
    bw: int,
    samp_rate: int,
    payload: str,
    cr: int,
    clean_dir: Path,
    air_dir: Path,
    channel_kwargs: Dict[str, object],
    regen: bool,
) -> Path:
    clean_dir.mkdir(parents=True, exist_ok=True)
    air_dir.mkdir(parents=True, exist_ok=True)
    cf32_path = air_dir / FILENAME
    if regen or not cf32_path.exists():
        export_args = build_export_args(
            sf, bw, samp_rate, payload, cr, clean_dir, air_dir, channel_kwargs
        )
        res = run_subprocess(export_args, check=False)
        if res.returncode != 0:
            raise RuntimeError(
                f"Failed to generate vector in {air_dir}:\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}"
            )
    return cf32_path


def run_decode_cli(binary: Path, cf32_path: Path, meta: Dict) -> Dict:
    cmd = [
        str(binary),
        "--sf",
        str(meta["sf"]),
        "--bw",
        str(meta["bw"]),
        "--fs",
        str(meta.get("samp_rate") or meta.get("sample_rate")),
        str(cf32_path),
    ]
    channel = meta.get("channel") or {}
    if isinstance(channel, dict) and channel.get("sampling_offset_ppm") is not None:
        cmd.extend(["--ppm-offset", str(channel["sampling_offset_ppm"])])
    res = run_subprocess(cmd, check=False)
    status = "success" if res.returncode == 0 else "failed"
    attempted = [(cmd, res)]
    if res.returncode != 0 and "--ppm-offset" in cmd:
        idx = cmd.index("--ppm-offset")
        fallback_cmd = [c for i, c in enumerate(cmd) if i not in {idx, idx + 1}]
        fallback_res = run_subprocess(fallback_cmd, check=False)
        attempted.append((fallback_cmd, fallback_res))
        if fallback_res.returncode == 0:
            cmd = fallback_cmd
            res = fallback_res
            status = "success"
    return {
        "cmd": " ".join(cmd),
        "status": status,
        "stdout": res.stdout.strip(),
        "stderr": res.stderr.strip(),
        "attempts": [
            {
                "cmd": " ".join(attempt_cmd),
                "returncode": attempt_res.returncode,
            }
            for attempt_cmd, attempt_res in attempted
        ],
    }


def run_streaming_compare(vectors_dir: Path, chunk: int) -> Dict:
    report_path = vectors_dir / STREAMING_REPORT_NAME
    cmd = [
        sys.executable,
        "-m",
        COMPARE_SCRIPT,
        "--vectors",
        str(vectors_dir),
        "--output",
        str(report_path),
        "--json-fallback",
        "--streaming-chunk",
        str(chunk),
    ]
    res = run_subprocess(cmd, check=False)
    if res.returncode != 0:
        raise RuntimeError(
            f"compare_streaming_compat failed for {vectors_dir}:\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}"
        )
    return json.loads(report_path.read_text())


def load_metadata(cf32_path: Path) -> Dict:
    meta_path = cf32_path.with_suffix(".json")
    if not meta_path.exists():
        raise FileNotFoundError(f"Metadata sidecar not found: {meta_path}")
    return json.loads(meta_path.read_text())


def main() -> None:
    parser = argparse.ArgumentParser(description="Run channel regression sweeps and comparisons.")
    parser.add_argument("--regen", action="store_true", help="Regenerate vectors even if they exist")
    parser.add_argument(
        "--output",
        type=Path,
        default=REPO_ROOT / "results/channel_regression_summary.json",
        help="Path for aggregated JSON summary",
    )
    parser.add_argument(
        "--streaming-chunk",
        type=int,
        default=4096,
        help="Chunk size passed to compare_streaming_compat/decode_cli streaming mode",
    )
    args = parser.parse_args()

    decode_cli = resolve_decode_cli()

    summary: Dict[str, List[Dict]] = {
        "decode_cli": [],
        "streaming_reports": [],
    }

    # Sweep definitions -----------------------------------------------------
    sf = 8
    bw = 125000
    samp_rate = 125000
    payload = "yo yo yo whats up"
    cr = 4

    cfo_values = [0, 40, 80, 120, 160, 200]
    rayleigh_dopplers = [0.5, 1.0, 2.0, 3.0]
    sampling_grid = [(ppm, bits) for ppm in (0, 8, 15, 22) for bits in (12, 10, 8)]
    combo_grid: List[Tuple[int, float, int]] = [
        (cfo, drift, ppm)
        for cfo in (20, 40, 60)
        for drift in (0.0, 1.0, 2.0)
        for ppm in (0, 10, 20)
    ]
    multipath_profiles = [
        ("mp_short", {"multipath_taps": "1,0.35@4", "snr_db": 24}),
        ("mp_mid", {"multipath_taps": "1,0.18-0.08j,0.12@6", "cfo_hz": 30, "snr_db": 21}),
        ("mp_harsh", {"multipath_taps": "1,0.26-0.12j,0.18@5,0.1@8", "rayleigh_fading": True, "fading_doppler_hz": 2.2, "cfo_hz": 55, "sampling_offset_ppm": 8, "snr_db": 18}),
    ]
    synthetic_impairments = [
        ("syn_imp_1", {"source_dir": "synthetic_impairments/air", "filename": "tx_sf9_bw125000_cr3_crc1_impl0_ldro2_pay6.cf32"}),
        ("syn_imp_2", {"source_dir": "synthetic_impairments/air", "filename": "tx_sf10_bw125000_cr4_crc1_impl0_ldro2_pay10.cf32"}),
        ("syn_imp_3", {"source_dir": "synthetic_impairments/air", "filename": "tx_sf8_bw250000_cr2_crc1_impl0_ldro2_pay11.cf32"}),
    ]
    custom_cases = [
        ("case1", {"cfo_hz": 30, "cfo_drift_hz": 0.5, "sampling_offset_ppm": 12, "phase_noise_lw": 2e-4, "multipath_taps": "1,0.2-0.1j,0.05@5", "snr_db": 18}),
        ("case2", {"cfo_hz": 70, "cfo_drift_hz": 0.0, "sampling_offset_ppm": 5, "phase_noise_lw": 1e-4, "multipath_taps": "1,0.15-0.05j", "rayleigh_fading": True, "fading_doppler_hz": 1.5, "quantize_bits": 11, "snr_db": 20}),
        ("case3", {"sampling_offset_ppm": 25, "phase_noise_lw": 5e-5, "clip": 0.9, "quantize_bits": 9, "snr_db": 22}),
        ("case4", {"cfo_hz": 45, "cfo_drift_hz": 1.8, "phase_noise_lw": 5e-4, "multipath_taps": "1,0.25-0.12j,0.08@6", "rayleigh_fading": True, "fading_doppler_hz": 3.0, "snr_db": 16}),
        ("case5", {"cfo_hz": 90, "cfo_drift_hz": 0.3, "sampling_offset_ppm": 15, "phase_noise_lw": 3e-4, "multipath_taps": "1,0.2j,0.05-0.02j", "rayleigh_fading": True, "fading_doppler_hz": 2.5, "quantize_bits": 10, "snr_db": 17}),
    ]

    vectors_root = REPO_ROOT / "vectors"
    synthetic_root = vectors_root / "synthetic_impairments" / "air"

    def process_vector(group: str, name: str, channel_kwargs: Dict[str, object]) -> None:
        clean_dir = vectors_root / f"{group}_clean" / name
        air_dir = vectors_root / f"{group}_air" / name
        cf32_path = ensure_vector(
            sf, bw, samp_rate, payload, cr, clean_dir, air_dir, channel_kwargs, regen=args.regen
        )
        meta = load_metadata(cf32_path)
        decode_result = run_decode_cli(decode_cli, cf32_path, meta)
        decode_result.update({"group": group, "case": name, "cf32": str(cf32_path.relative_to(REPO_ROOT))})
        summary["decode_cli"].append(decode_result)

        streaming_result = run_streaming_compare(air_dir, args.streaming_chunk)
        streaming_result.update({"group": group, "case": name})
        summary["streaming_reports"].append(streaming_result)

    for cfo in cfo_values:
        process_vector("sweep_cfo", f"cfo_{cfo}", {"cfo_hz": cfo, "snr_db": 25})

    for doppler in rayleigh_dopplers:
        process_vector("sweep_rayleigh", f"dop_{doppler}", {"rayleigh_fading": True, "fading_doppler_hz": doppler, "snr_db": 20})

    for ppm, bits in sampling_grid:
        process_vector("sweep_sampling", f"ppm_{ppm}_bits_{bits}", {"sampling_offset_ppm": ppm, "quantize_bits": bits, "snr_db": 25})

    for cfo, drift, ppm in combo_grid:
        process_vector(
            "sweep_combo",
            f"cfo{cfo}_dr{drift}_ppm{ppm}",
            {"cfo_hz": cfo, "cfo_drift_hz": drift, "sampling_offset_ppm": ppm, "snr_db": 25, "phase_noise_lw": 5e-4},
        )

    for profile_name, params in multipath_profiles:
        enriched = dict(params)
        enriched.setdefault("snr_db", 22)
        process_vector("sweep_multipath", profile_name, enriched)

    for case_name, params in custom_cases:
        process_vector("new_cases", case_name, params)

    for imp_name, params in synthetic_impairments:
        src = synthetic_root / params["filename"]
        if not src.exists():
            print(f"[warn] synthetic impairment vector missing: {src}, skipping")
            continue
        clean_dir = vectors_root / "synthetic_impairments_clean" / imp_name
        air_dir = vectors_root / "synthetic_impairments_air" / imp_name
        clean_dir.mkdir(parents=True, exist_ok=True)
        air_dir.mkdir(parents=True, exist_ok=True)
        target_clean = clean_dir / params["filename"]
        target_air = air_dir / params["filename"]
        target_clean.write_bytes(src.read_bytes())
        target_air.write_bytes(src.read_bytes())
        meta_src = src.with_suffix(".json")
        target_clean.with_suffix(".json").write_bytes(meta_src.read_bytes())
        target_air.with_suffix(".json").write_bytes(meta_src.read_bytes())

        meta = load_metadata(target_air)
        decode_result = run_decode_cli(decode_cli, target_air, meta)
        decode_result.update({"group": "synthetic_impairments", "case": imp_name, "cf32": str(target_air.relative_to(REPO_ROOT))})
        summary["decode_cli"].append(decode_result)

        streaming_result = run_streaming_compare(air_dir, args.streaming_chunk)
        streaming_result.update({"group": "synthetic_impairments", "case": imp_name})
        summary["streaming_reports"].append(streaming_result)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2))
    print(f"Wrote channel regression summary to {args.output}")


if __name__ == "__main__":
    main()
