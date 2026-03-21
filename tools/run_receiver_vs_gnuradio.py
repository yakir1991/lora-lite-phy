#!/usr/bin/env python3

"""
Compare the standalone host_sim receiver against the GNU Radio gr-lora_sdr reference
across multiple captures and impairment profiles.

For each capture/profile pair we:
  1. Apply the requested impairments (CFO, STO, SFO) to the IQ samples.
  2. Decode with GNU Radio (via tools/gr_decode_capture.py) and measure runtime.
  3. Decode with host_sim/lora_replay (optionally collecting its summary JSON).
Outputs: consolidated JSON plus optional Markdown report ready for docs.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

import numpy as np
from numpy.random import default_rng

REPO_ROOT = Path(__file__).resolve().parent.parent
COLLISION_WAVEFORMS: Dict[str, np.ndarray] = {}


@dataclass
class RunMetrics:
    status: str
    wall_s: float
    user_s: float
    sys_s: float
    stdout: str
    stderr: str
    extra: Dict[str, Any]


def load_cf32(path: Path) -> np.ndarray:
    return np.fromfile(path, dtype=np.complex64)


def save_cf32(path: Path, samples: np.ndarray) -> None:
    samples.astype(np.complex64).tofile(path)


def load_collision_waveform(path: str) -> np.ndarray:
    resolved = str(Path(path).expanduser().resolve())
    wave = COLLISION_WAVEFORMS.get(resolved)
    if wave is None:
        data = np.fromfile(resolved, dtype=np.complex64)
        wave = data if data.size else np.zeros(1, dtype=np.complex64)
        COLLISION_WAVEFORMS[resolved] = wave
    return wave


def apply_sto(samples: np.ndarray, sto_samples: int) -> np.ndarray:
    if sto_samples == 0:
        return samples
    if sto_samples > 0:
        pad = np.zeros(sto_samples, dtype=np.complex64)
        return np.concatenate([pad, samples[:-sto_samples]]) if sto_samples < len(samples) else np.concatenate([pad, np.zeros(1, dtype=np.complex64)])
    # negative shift = advance (drop samples at start)
    advance = abs(sto_samples)
    if advance >= len(samples):
        return np.zeros_like(samples)
    pad = np.zeros(advance, dtype=np.complex64)
    return np.concatenate([samples[advance:], pad])


def apply_cfo(samples: np.ndarray, sample_rate: float, cfo_ppm: float) -> np.ndarray:
    if abs(cfo_ppm) < 1e-12:
        return samples
    freq_hz = sample_rate * (cfo_ppm * 1e-6)
    n = np.arange(len(samples), dtype=np.float64)
    phase = 2.0 * math.pi * freq_hz * n / sample_rate
    rotation = np.exp(1j * phase)
    return samples * rotation.astype(np.complex64)


def resample_linear(samples: np.ndarray, ratio: float) -> np.ndarray:
    if ratio <= 0.0 or abs(ratio - 1.0) < 1e-9:
        return samples
    length = max(1, int(round(len(samples) * ratio)))
    src_idx = np.arange(len(samples), dtype=np.float64)
    dst_idx = np.linspace(0.0, len(samples) - 1.0, length)
    real = np.interp(dst_idx, src_idx, samples.real)
    imag = np.interp(dst_idx, src_idx, samples.imag)
    return (real + 1j * imag).astype(np.complex64)


def resample_with_drift(samples: np.ndarray,
                        ppm_start: float,
                        ppm_end: float,
                        segments: int = 16) -> np.ndarray:
    if samples.size == 0:
        return samples
    if abs(ppm_start) < 1e-9 and abs(ppm_end) < 1e-9:
        return samples
    seg_count = max(1, min(segments, samples.size))
    chunk = max(1, int(math.ceil(samples.size / seg_count)))
    out_chunks: List[np.ndarray] = []
    for seg in range(seg_count):
        start = seg * chunk
        if start >= samples.size:
            break
        end = min(samples.size, start + chunk)
        frac = seg / max(1, seg_count - 1)
        ppm = ppm_start + (ppm_end - ppm_start) * frac
        ratio = 1.0 + ppm * 1e-6
        segment = samples[start:end]
        if abs(ratio - 1.0) < 1e-9 or segment.size < 2:
            out_chunks.append(segment)
        else:
            out_chunks.append(resample_linear(segment, ratio))
    return np.concatenate(out_chunks) if out_chunks else samples


def apply_impairments(
    base_samples: np.ndarray,
    sample_rate: float,
    profile: Dict[str, Any],
    rng_seed: Optional[int] = None,
) -> np.ndarray:
    out = np.copy(base_samples)
    cfo_ppm = float(profile.get("cfo_ppm", 0.0))
    sto_samples = int(profile.get("sto_samples", 0))
    sfo_ppm = float(profile.get("sfo_ppm", 0.0))
    seed = int(rng_seed if rng_seed is not None else profile.get("seed", 12345))
    rng = default_rng(seed)

    if sto_samples != 0:
        out = apply_sto(out, sto_samples)
    if abs(cfo_ppm) > 0.0:
        out = apply_cfo(out, sample_rate, cfo_ppm)
    cfo_start = profile.get("cfo_ppm_start")
    cfo_end = profile.get("cfo_ppm_end")
    if cfo_start is not None and cfo_end is not None and out.size > 0:
        ppm_series = np.linspace(float(cfo_start), float(cfo_end), out.size, dtype=np.float64)
        phase = np.cumsum(2.0 * np.pi * ppm_series * 1e-6, dtype=np.float64)
        out *= np.exp(1j * phase).astype(np.complex64)
    if abs(sfo_ppm) > 0.0:
        ratio = 1.0 + sfo_ppm * 1e-6
        out = resample_linear(out, ratio)
    sfo_start = profile.get("sfo_ppm_start")
    sfo_end = profile.get("sfo_ppm_end")
    if sfo_start is not None and sfo_end is not None:
        out = resample_with_drift(out, float(sfo_start), float(sfo_end))

    def add_awgn(segment: np.ndarray, snr_db: float) -> None:
        if segment.size == 0:
            return
        power = np.mean(np.abs(segment) ** 2)
        power = max(power, 1e-12)
        snr_linear = 10 ** (snr_db / 10.0)
        sigma = np.sqrt(power / (2.0 * snr_linear))
        noise = (rng.normal(size=segment.size) + 1j * rng.normal(size=segment.size)) * sigma
        segment += noise.astype(np.complex64)

    awgn_snr = profile.get("awgn_snr_db")
    if awgn_snr is not None:
        add_awgn(out, float(awgn_snr))

    burst = profile.get("burst")
    if burst:
        period = int(burst.get("period_samples", len(out)))
        duration = int(burst.get("duration_samples", period))
        snr = float(burst.get("snr_db", 0.0))
        for start in range(0, len(out), period):
            end = min(start + duration, len(out))
            if start >= end:
                break
            add_awgn(out[start:end], snr)

    iq_gain_i = float(profile.get("iq_gain_i", 1.0))
    iq_gain_q = float(profile.get("iq_gain_q", 1.0))
    iq_phase_deg = float(profile.get("iq_phase_deg", 0.0))
    if (abs(iq_gain_i - 1.0) > 1e-4) or (abs(iq_gain_q - 1.0) > 1e-4) or (abs(iq_phase_deg) > 1e-4):
        i = out.real * iq_gain_i
        q = out.imag * iq_gain_q
        if abs(iq_phase_deg) > 1e-9:
            phase = math.radians(iq_phase_deg)
            cos_p = math.cos(phase)
            sin_p = math.sin(phase)
            i_rot = i * cos_p - q * sin_p
            q_rot = i * sin_p + q * cos_p
            i, q = i_rot, q_rot
        out = (i + 1j * q).astype(np.complex64)

    collision = profile.get("collision")
    if collision:
        waveform_path = collision.get("waveform")
        if waveform_path:
            wave = load_collision_waveform(waveform_path)
            if wave.size > 0:
                scale = float(collision.get("scale", 1.0))
                period = int(collision.get("period_samples", len(out)))
                duration = int(collision.get("duration_samples", period))
                offset = 0
                for start in range(0, len(out), period):
                    end = min(start + duration, len(out))
                    if start >= end:
                        break
                    seg_len = end - start
                    if offset + seg_len > wave.size:
                        tiles = (seg_len // wave.size) + 1
                        overlay = np.tile(wave, tiles)[:seg_len]
                    else:
                        overlay = wave[offset: offset + seg_len]
                    out[start:end] += (scale * overlay).astype(np.complex64)
                    offset = (offset + seg_len) % wave.size

    return out


def parse_time_metrics(stderr: str) -> tuple[Dict[str, float], str]:
    stats: Dict[str, float] = {}
    filtered_lines: List[str] = []

    def parse_elapsed(token: str) -> float:
        token = token.strip()
        parts = token.split(':')
        if len(parts) == 3:
            hours, minutes, seconds = parts
        elif len(parts) == 2:
            hours = '0'
            minutes, seconds = parts
        else:
            return float(token)
        return int(hours) * 3600 + int(minutes) * 60 + float(seconds)

    for line in stderr.splitlines():
        stripped = line.strip()
        if ': ' not in stripped:
            filtered_lines.append(line)
            continue
        key, value = stripped.split(':', 1)
        key = key.strip()
        value = value.strip()
        if key == 'Maximum resident set size (kbytes)':
            try:
                stats['max_rss_kb'] = float(value)
            except ValueError:
                pass
        elif key == 'Percent of CPU this job got':
            try:
                stats['cpu_percent'] = float(value.strip().strip('%'))
            except ValueError:
                pass
        elif key == 'Elapsed (wall clock) time (h:mm:ss or m:ss)':
            try:
                stats['elapsed_wall_s'] = parse_elapsed(value)
            except ValueError:
                pass
        elif key == 'User time (seconds)':
            try:
                stats['user_s_reported'] = float(value)
            except ValueError:
                pass
        elif key == 'System time (seconds)':
            try:
                stats['sys_s_reported'] = float(value)
            except ValueError:
                pass
        else:
            filtered_lines.append(line)
    filtered_stderr = '\n'.join(filtered_lines)
    return stats, filtered_stderr


def run_subprocess(
    cmd: List[str],
    cwd: Optional[Path] = None,
    env: Optional[Dict[str, str]] = None,
    timeout_s: Optional[float] = None,
) -> RunMetrics:
    time_tool = Path('/usr/bin/time')
    use_time = time_tool.exists()
    run_cmd = ([str(time_tool), '-v'] + cmd) if use_time else cmd
    start_wall = time.perf_counter()
    try:
        proc = subprocess.run(
            run_cmd,
            cwd=str(cwd) if cwd else None,
            capture_output=True,
            text=True,
            encoding='utf-8',
            errors='replace',
            env=env,
            timeout=timeout_s,
        )
        end_wall = time.perf_counter()
        status = 'ok' if proc.returncode == 0 else f'error({proc.returncode})'
        stdout = proc.stdout or ''
        stderr = proc.stderr or ''
    except subprocess.TimeoutExpired as exc:
        end_wall = time.perf_counter()
        status = f'timeout({timeout_s}s)'
        stdout = (exc.stdout or '') if isinstance(exc.stdout, str) else ''
        stderr = (exc.stderr or '') if isinstance(exc.stderr, str) else ''
    stats = {}
    if use_time and stderr:
        stats, stderr = parse_time_metrics(stderr)
    wall_s = stats.get('elapsed_wall_s', end_wall - start_wall)
    user_s = stats.get('user_s_reported', 0.0)
    sys_s = stats.get('sys_s_reported', 0.0)
    if not use_time:
        user_s = 0.0
        sys_s = 0.0
    return RunMetrics(
        status=status,
        wall_s=wall_s,
        user_s=user_s,
        sys_s=sys_s,
        stdout=stdout,
        stderr=stderr,
        extra=stats,
    )


def ensure_env_var(name: str) -> str:
    value = os.environ.get(name)
    if value is None:
        raise RuntimeError(f"Environment variable {name} is required")
    return value


def read_payload_hex(path: Path) -> Optional[str]:
    if not path.exists():
        return None
    data = path.read_bytes()
    if not data:
        return None
    return data.hex()


def compute_gn_timeout(
    sample_rate: float,
    sample_count: int,
    sf: int,
    base_timeout: Optional[float],
) -> Optional[float]:
    if base_timeout is None:
        return None
    if sample_rate <= 0.0 or sample_count <= 0:
        return base_timeout
    expected_s = float(sample_count) / sample_rate
    # Keep the sweep bounded in wall-clock time. Older logic attempted to
    # scale timeouts aggressively for low SF, but that can explode to
    # many minutes per tuple on long captures.
    _ = expected_s
    _ = sf
    return base_timeout


def decode_with_gnuradio(
    env_name: str,
    script_path: Path,
    iq_path: Path,
    metadata_path: Path,
    payload_out: Path,
    timeout_s: Optional[float],
) -> RunMetrics:
    meta = json.loads(metadata_path.read_text())
    expected_len = int(meta.get("payload_len", 0))
    env = os.environ.copy()
    env.setdefault("CONDA_OVERRIDE_CUDA", "0")

    # Default to in-tree gr_lora_sdr to match the capture generator environment.
    # Opt out with GRLORA_USE_IN_TREE=0 if you want the conda-installed blocks.
    use_in_tree = os.environ.get("GRLORA_USE_IN_TREE", "1") not in ("0", "false", "False")
    if use_in_tree:
        python_paths: List[str] = []
        install_lib = REPO_ROOT / "gr_lora_sdr" / "install" / "lib"
        if install_lib.exists():
            for candidate in sorted(install_lib.glob("python*/site-packages")):
                python_paths.append(str(candidate.resolve()))
            existing_ld = env.get("LD_LIBRARY_PATH")
            env["LD_LIBRARY_PATH"] = (
                f"{install_lib}:{existing_ld}" if existing_ld else str(install_lib)
            )
        python_src = REPO_ROOT / "gr_lora_sdr" / "python"
        if python_src.exists():
            python_paths.append(str(python_src.resolve()))
        existing_py = env.get("PYTHONPATH")
        if python_paths:
            env["PYTHONPATH"] = ":".join(python_paths + ([existing_py] if existing_py else []))

    # Avoid extremely verbose tracing unless explicitly enabled by the user.
    env.setdefault("GRLORA_TRACE_BUFFERS_INTERVAL", "0")

    base_cmd = [
        "conda",
        "run",
        "-n",
        env_name,
        "python",
        str(script_path),
        "--input",
        str(iq_path),
        "--metadata",
        str(metadata_path),
        "--payload-out",
        str(payload_out),
        "--force-manual",
    ]

    try:
        max_attempts = int(os.environ.get("GRLORA_GN_ATTEMPTS", "10"))
    except ValueError:
        max_attempts = 10
    max_attempts = max(1, min(50, max_attempts))

    def run_decode(
        extra_env: Optional[Dict[str, str]] = None,
        extra_args: Optional[List[str]] = None,
    ) -> RunMetrics:
        # Avoid stale payload bytes across retries (normal/fallback/bypass).
        try:
            payload_out.unlink(missing_ok=True)
        except TypeError:
            # Python < 3.8 compatibility
            if payload_out.exists():
                payload_out.unlink()
        run_env = env.copy()
        if extra_env:
            run_env.update(extra_env)
        cmd = list(base_cmd)
        if extra_args:
            cmd.extend(extra_args)
        return run_subprocess(cmd, env=run_env, timeout_s=timeout_s)

    def read_payload(result: RunMetrics) -> None:
        if result.status != "ok":
            return
        payload_bytes = 0
        payload_hex = None
        if payload_out.exists():
            data = payload_out.read_bytes()
            if expected_len > 0:
                data = data[:expected_len]
            payload_bytes = len(data)
            payload_hex = data.hex() if payload_bytes else None
        result.extra.update(
            {
                "payload_hex": payload_hex,
                "payload_bytes": payload_bytes,
            }
        )

    def has_full_payload(result: RunMetrics) -> bool:
        if result.status != "ok":
            return False
        pb = int(result.extra.get("payload_bytes") or 0)
        if pb <= 0:
            return False
        return expected_len <= 0 or pb >= expected_len

    def run_with_retries(
        extra_env: Optional[Dict[str, str]] = None,
        extra_args: Optional[List[str]] = None,
        attempts: int = 1,
    ) -> RunMetrics:
        last: Optional[RunMetrics] = None
        for attempt in range(1, attempts + 1):
            result = run_decode(extra_env, extra_args)
            read_payload(result)
            result.extra["attempt"] = attempt
            last = result
            if has_full_payload(result):
                return result
        return last if last is not None else run_decode(extra_env, extra_args)

    result = run_with_retries(attempts=max_attempts)
    if has_full_payload(result):
        return result

    # Retry with metadata fallback when header decode or CRC gating produce no payload.
    fallback = run_with_retries(
        {
            "GRLORA_HEADER_METADATA_FALLBACK": "1",
            "GRLORA_ACCEPT_FALLBACK_HEADER": "1",
        },
        attempts=max_attempts,
    )
    if has_full_payload(fallback):
        return fallback

    # Last resort: bypass CRC verification to emit bytes for comparison.
    bypass = run_decode(
        {
            "GRLORA_HEADER_METADATA_FALLBACK": "1",
            "GRLORA_ACCEPT_FALLBACK_HEADER": "1",
        },
        ["--bypass-crc-verif"],
    )
    bypass.extra["crc_bypassed"] = True
    read_payload(bypass)
    return bypass


def decode_with_lora(
    lora_replay: Path,
    iq_path: Path,
    metadata_path: Path,
    summary_path: Path,
    dump_payload_path: Path,
    timeout_s: Optional[float],
    force_bypass_crc: bool = False,
) -> RunMetrics:
    meta = json.loads(metadata_path.read_text())
    expected_len = int(meta.get("payload_len", 0))

    def run_decode(extra_args: Optional[List[str]] = None) -> RunMetrics:
        cmd = [
            str(lora_replay),
            "--iq",
            str(iq_path),
            "--metadata",
            str(metadata_path),
            "--summary",
            str(summary_path),
            "--dump-payload",
            str(dump_payload_path),
            "--ignore-ref-mismatch",
        ]
        if force_bypass_crc:
            cmd.append("--bypass-crc-verif")
        if extra_args:
            cmd.extend(extra_args)
        result = run_subprocess(cmd, timeout_s=timeout_s)
        if result.status == "ok" and summary_path.exists():
            summary = json.loads(summary_path.read_text())
            result.extra.update(summary)
        result.extra["payload_hex"] = read_payload_hex(dump_payload_path)
        return result

    result = run_decode()
    if (
        result.status == "ok"
        and result.extra.get("payload_hex")
        and (expected_len <= 0 or len(result.extra.get("payload_hex", "")) >= expected_len * 2)
    ):
        if force_bypass_crc:
            result.extra["crc_bypassed"] = True
        return result

    if force_bypass_crc:
        # We already asked the binary to bypass CRC; don't run an additional retry.
        result.extra["crc_bypassed"] = True
        return result

    bypass = run_decode(["--bypass-crc-verif"])
    bypass.extra["crc_bypassed"] = True
    return bypass


def build_markdown(results: List[Dict[str, Any]]) -> str:
    lines = [
        "# Standalone vs GNU Radio Comparison",
        "",
        "| Capture | Profile | CFO ppm | STO samples | SFO ppm | GNU Radio (ms) | Standalone (ms) | Notes |",
        "| --- | --- | ---:| ---:| ---:| ---:| ---:| --- |",
    ]
    for item in results:
        impair = item["impairment"]
        gn = item["gnuradio"]
        host = item["standalone"]
        lines.append(
            f"| {item['capture']} | {item['profile']} | "
            f"{impair.get('cfo_ppm', 0.0):+.1f} | {impair.get('sto_samples', 0)} | "
            f"{impair.get('sfo_ppm', 0.0):+.1f} | "
            f"{gn['wall_s'] * 1e3:.1f} | {host['wall_s'] * 1e3:.1f} | "
            f"{gn['status']}/{host['status']} |"
        )
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", type=Path, help="Comparison matrix JSON (captures + impairment profiles)")
    parser.add_argument(
        "--capture-root",
        type=Path,
        default=Path("gr_lora_sdr/data/generated"),
        help="Root folder for capture CF32 files",
    )
    parser.add_argument(
        "--work-dir",
        type=Path,
        default=Path("build/receiver_vs_gnuradio"),
        help="Temporary working directory for impaired IQ / payloads",
    )
    parser.add_argument("--output-json", type=Path, required=True, help="Summary JSON output")
    parser.add_argument("--markdown", type=Path, help="Optional Markdown report path")
    parser.add_argument(
        "--captures",
        nargs="*",
        help="Optional subset of capture names to process (default: all in matrix)",
    )
    parser.add_argument(
        "--lora-replay",
        type=Path,
        default=Path("build/host_sim/lora_replay"),
        help="Path to lora_replay binary",
    )
    parser.add_argument(
        "--gnuradio-env",
        type=str,
        default="gr310",
        help="Conda environment name that hosts GNU Radio",
    )
    parser.add_argument(
        "--gr-script",
        type=Path,
        default=Path("tools/gr_decode_capture.py"),
        help="Path to the GNU Radio decode helper script",
    )
    parser.add_argument(
        "--mc-runs",
        type=int,
        default=1,
        help="Monte Carlo iterations per capture/profile (seed offset applied when >1)",
    )
    parser.add_argument(
        "--mc-base-seed",
        type=int,
        default=1000,
        help="Base offset added to profile seeds for Monte Carlo runs",
    )
    parser.add_argument(
        "--timeout-s",
        type=float,
        default=float(os.environ.get("LORA_COMPARE_TIMEOUT_S", "120")),
        help="Timeout (seconds) for each decoder subprocess (GNU Radio and standalone). Set 0 to disable.",
    )
    args = parser.parse_args()

    timeout_s: Optional[float] = None
    if args.timeout_s and args.timeout_s > 0:
        timeout_s = float(args.timeout_s)

    if args.mc_runs < 1:
        raise SystemExit("--mc-runs must be >= 1")

    matrix = json.loads(args.matrix.read_text())
    captures = matrix.get("captures", [])
    profiles = matrix.get("impairment_profiles", [])
    if not captures or not profiles:
        raise SystemExit("Matrix must define 'captures' and 'impairment_profiles'")

    work_dir = args.work_dir
    work_dir.mkdir(parents=True, exist_ok=True)

    results: List[Dict[str, Any]] = []

    capture_filter = set(args.captures or [])

    for capture_entry in captures:
        capture_name = capture_entry["name"]
        if capture_filter and capture_name not in capture_filter:
            continue
        capture_rel = Path(capture_entry["capture"])
        metadata_rel = Path(capture_entry["metadata"])

        if capture_rel.is_absolute():
            capture_path = capture_rel
        elif capture_rel.exists():
            capture_path = capture_rel
        else:
            capture_path = args.capture_root / capture_rel
        capture_path = capture_path.resolve()

        if metadata_rel.is_absolute():
            metadata_path = metadata_rel
        elif metadata_rel.exists():
            metadata_path = metadata_rel
        else:
            metadata_path = args.capture_root / metadata_rel
        metadata_path = metadata_path.resolve()
        if not capture_path.exists():
            raise FileNotFoundError(f"Capture not found: {capture_path}")
        if not metadata_path.exists():
            raise FileNotFoundError(f"Metadata not found: {metadata_path}")

        base_samples = load_cf32(capture_path)
        tail_samples = capture_entry.get("tail_samples")
        sample_offset = capture_entry.get("sample_offset")
        if isinstance(tail_samples, int) and tail_samples > 0:
            sample_offset = max(0, len(base_samples) - tail_samples)
        if isinstance(sample_offset, int) and sample_offset > 0:
            if sample_offset < len(base_samples):
                base_samples = base_samples[sample_offset:]
            else:
                base_samples = np.zeros(1, dtype=np.complex64)
        max_samples = capture_entry.get("max_samples")
        if isinstance(max_samples, int) and max_samples > 0:
            if max_samples < len(base_samples):
                base_samples = base_samples[:max_samples]
        meta = json.loads(metadata_path.read_text())
        override = capture_entry.get("override") or {}
        for key, value in override.items():
            meta[key] = value
        sample_rate = float(meta.get("sample_rate") or meta.get("sample_rate_hz") or 0.0)
        if sample_rate <= 0.0:
            raise RuntimeError(f"Metadata missing sample_rate: {metadata_path}")

        allowed_profiles = capture_entry.get("profiles")
        for profile in profiles:
            if allowed_profiles and profile["label"] not in allowed_profiles:
                continue
            profile_label = profile["label"]
            base_seed = int(profile.get("seed", 12345))
            mc_runs = max(1, args.mc_runs)
            for mc_index in range(mc_runs):
                if args.mc_runs > 1:
                    seed_used = base_seed + args.mc_base_seed + mc_index
                else:
                    seed_used = base_seed
                suffix = f"{capture_name}_{profile_label}"
                if args.mc_runs > 1:
                    suffix += f"_mc{mc_index}"

                impaired_samples = apply_impairments(base_samples, sample_rate, profile, rng_seed=seed_used)
                impaired_path = work_dir / f"{suffix}.cf32"
                save_cf32(impaired_path, impaired_samples)

                payload_out = work_dir / f"{suffix}_gnuradio_payload.bin"
                gn_timeout = compute_gn_timeout(sample_rate, len(impaired_samples), int(meta.get("sf", 7)), timeout_s)
                gn_metrics = decode_with_gnuradio(
                    args.gnuradio_env,
                    args.gr_script.resolve(),
                    impaired_path,
                    metadata_path,
                    payload_out,
                    gn_timeout,
                )

                summary_path = work_dir / f"{suffix}_standalone_summary.json"
                host_payload_dump = work_dir / f"{suffix}_standalone_payload.bin"
                host_metrics = decode_with_lora(
                    args.lora_replay.resolve(),
                    impaired_path,
                    metadata_path,
                    summary_path,
                    host_payload_dump,
                    timeout_s,
                    force_bypass_crc=bool(gn_metrics.extra.get("crc_bypassed")),
                )

                results.append(
                    {
                        "capture": capture_name,
                        "profile": profile_label,
                        "mc_iteration": mc_index,
                        "mc_seed": seed_used,
                        "impairment": {
                            "cfo_ppm": float(profile.get("cfo_ppm", 0.0)),
                            "sto_samples": int(profile.get("sto_samples", 0)),
                            "sfo_ppm": float(profile.get("sfo_ppm", 0.0)),
                            "cfo_ppm_start": float(profile.get("cfo_ppm_start", 0.0))
                            if "cfo_ppm_start" in profile else None,
                            "cfo_ppm_end": float(profile.get("cfo_ppm_end", 0.0))
                            if "cfo_ppm_end" in profile else None,
                            "sfo_ppm_start": float(profile.get("sfo_ppm_start", 0.0))
                            if "sfo_ppm_start" in profile else None,
                            "sfo_ppm_end": float(profile.get("sfo_ppm_end", 0.0))
                            if "sfo_ppm_end" in profile else None,
                            "iq_gain_i": float(profile.get("iq_gain_i", 1.0))
                            if "iq_gain_i" in profile else None,
                            "iq_gain_q": float(profile.get("iq_gain_q", 1.0))
                            if "iq_gain_q" in profile else None,
                            "iq_phase_deg": float(profile.get("iq_phase_deg", 0.0))
                            if "iq_phase_deg" in profile else None,
                        },
                        "samples_written": len(impaired_samples),
                        "gnuradio": {
                            "status": gn_metrics.status,
                            "wall_s": gn_metrics.wall_s,
                            "user_s": gn_metrics.user_s,
                            "sys_s": gn_metrics.sys_s,
                            "payload_bytes": gn_metrics.extra.get("payload_bytes"),
                            "crc_bypassed": bool(gn_metrics.extra.get("crc_bypassed")),
                            "resources": {
                                "max_rss_kb": gn_metrics.extra.get("max_rss_kb"),
                                "cpu_percent": gn_metrics.extra.get("cpu_percent"),
                            },
                        },
                        "standalone": {
                            "status": host_metrics.status,
                            "wall_s": host_metrics.wall_s,
                            "user_s": host_metrics.user_s,
                            "sys_s": host_metrics.sys_s,
                            "crc_bypassed": bool(host_metrics.extra.get("crc_bypassed")),
                            "resources": {
                                "max_rss_kb": host_metrics.extra.get("max_rss_kb"),
                                "cpu_percent": host_metrics.extra.get("cpu_percent"),
                            },
                            "summary": host_metrics.extra,
                        },
                        "payload": {
                            "expected_hex": gn_metrics.extra.get("payload_hex"),
                            "standalone_hex": host_metrics.extra.get("payload_hex"),
                        },
                        "payload_match": (
                            gn_metrics.extra.get("payload_hex") is not None
                            and host_metrics.extra.get("payload_hex") is not None
                            and gn_metrics.extra.get("payload_hex").lower()
                            == host_metrics.extra.get("payload_hex").lower()
                        )
                        if gn_metrics.extra.get("payload_hex") is not None
                        and host_metrics.extra.get("payload_hex") is not None
                        else None,
                    }
                )

    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(results, indent=2) + "\n")

    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(build_markdown(results))

    print(f"Wrote comparison results to {args.output_json}")
    if args.markdown:
        print(f"Wrote Markdown report to {args.markdown}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
