#!/usr/bin/env python3
"""
make_hard_vector.py

Generate a very challenging LoRa CF32 vector by starting from a clean TX
frame (provided or generated) and applying realistic channel impairments:
 - AWGN (target SNR dB)
 - CFO (fractional frequency offset)
 - Clock drift (ppm) via resampling
 - Time offset (prepend noise)
 - IQ imbalance (gain/phase mismatch)
 - DC offset (complex)
 - Multipath (complex FIR taps)

The output is a .cf32 file and a sidecar .json with parameters.

Notes:
 - You can pass --input to use any existing clean vector.
 - Or use --emit-with-gr to call external/gr_lora_sdr/scripts/generate_golden_vectors.py
   to synthesize a clean frame, then apply impairments.
 - No changes to external packages are required.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

import numpy as np

try:
    # Optional for clock-drift resampling
    from scipy.signal import resample_poly  # type: ignore
except Exception:
    resample_poly = None  # type: ignore


def _load_cf32(path: Path) -> np.ndarray:
    data = np.fromfile(path, dtype=np.complex64)
    if data.size == 0:
        raise ValueError(f"Empty IQ file: {path}")
    return data.astype(np.complex64, copy=False)


def _save_cf32(path: Path, iq: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    iq.astype(np.complex64).tofile(path)


def _power_avg(x: np.ndarray) -> float:
    return float(np.mean(np.abs(x) ** 2))


def impair_awgn(iq: np.ndarray, snr_db: float) -> np.ndarray:
    if math.isinf(snr_db):
        return iq
    p_sig = _power_avg(iq)
    snr_lin = 10 ** (snr_db / 10.0)
    p_noise = p_sig / max(snr_lin, 1e-12)
    sigma = math.sqrt(p_noise / 2.0)
    noise = (np.random.normal(0, sigma, iq.shape) + 1j * np.random.normal(0, sigma, iq.shape)).astype(
        np.complex64
    )
    return (iq + noise).astype(np.complex64)


def impair_cfo(iq: np.ndarray, cfo_frac: float) -> np.ndarray:
    """Apply CFO as a fraction of sample rate (cycles per sample). For example, 0.001 ~ 0.1% of fs."""
    if abs(cfo_frac) < 1e-12:
        return iq
    n = np.arange(iq.size, dtype=np.float64)
    rot = np.exp(1j * 2.0 * np.pi * cfo_frac * n).astype(np.complex64)
    return (iq * rot).astype(np.complex64)


def impair_clock_drift(iq: np.ndarray, ppm: float) -> np.ndarray:
    """Simulate clock drift by resampling with factor (1 + ppm*1e-6)."""
    if abs(ppm) < 1e-9:
        return iq
    factor = 1.0 + ppm * 1e-6
    if resample_poly is None:
        # Fallback: linear interpolation grid
        idx = np.arange(iq.size, dtype=np.float64)
        new_len = int(round(iq.size * factor))
        new_idx = np.linspace(0, iq.size - 1, new_len)
        # Interpolate I and Q separately to keep speed reasonable
        i = np.interp(new_idx, idx, iq.real)
        q = np.interp(new_idx, idx, iq.imag)
        return (i + 1j * q).astype(np.complex64)
    # Use rational approximation for efficiency
    # Represent factor ~ p/q with limited denominator
    from fractions import Fraction

    f = Fraction(factor).limit_denominator(1000)
    up, down = f.numerator, f.denominator
    y = resample_poly(iq, up, down)
    return y.astype(np.complex64)


def impair_time_offset(iq: np.ndarray, extra_noise_samps: int, snr_db: float) -> np.ndarray:
    if extra_noise_samps <= 0:
        return iq
    # Create noise-only prefix at same SNR relative to unit power
    prefix = impair_awgn(np.zeros(extra_noise_samps, dtype=np.complex64), snr_db)
    return np.concatenate([prefix, iq]).astype(np.complex64)


def impair_dc(iq: np.ndarray, dc_real: float, dc_imag: float) -> np.ndarray:
    if abs(dc_real) < 1e-12 and abs(dc_imag) < 1e-12:
        return iq
    return (iq + (dc_real + 1j * dc_imag)).astype(np.complex64)


def impair_iq_imbalance(iq: np.ndarray, gain_db: float, phase_deg: float) -> np.ndarray:
    if abs(gain_db) < 1e-9 and abs(phase_deg) < 1e-9:
        return iq
    g = 10 ** (gain_db / 20.0)
    phi = math.radians(phase_deg)
    # Approximate IQ imbalance as a 2x2 mixing of I/Q components
    I = iq.real
    Q = iq.imag
    # Gain mismatch on I, phase skew on Q
    I2 = g * I
    Q2 = Q * math.cos(phi) + I * math.sin(phi)
    return (I2 + 1j * Q2).astype(np.complex64)


def impair_multipath(iq: np.ndarray, taps: Sequence[complex]) -> np.ndarray:
    if not taps:
        return iq
    h = np.asarray(taps, dtype=np.complex64)
    y = np.convolve(iq, h, mode="full")
    return y.astype(np.complex64)


def run_gr_generate(
    *,
    out_dir: Path,
    sf: int,
    bw: int,
    samp_rate: int,
    cr: int,
    payload: bytes,
    has_crc: bool,
    impl_header: bool,
    ldro: int,
    module_path: Optional[str],
) -> Tuple[Path, dict]:
    script = Path("external/gr_lora_sdr/scripts/generate_golden_vectors.py").resolve()
    if not script.exists():
        raise FileNotFoundError(script)
    out_dir = out_dir.expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    args = [
        sys.executable,
        str(script),
        "--emit-frame",
        "--sf",
        str(sf),
        "--bw",
        str(bw),
        "--samp-rate",
        str(samp_rate),
        "--cr",
        str(cr),
        "--ldro-mode",
        str(ldro),
        "--payload",
        payload.hex() if any(b < 32 or b >= 127 for b in payload) else payload.decode("latin-1"),
        "--out-dir",
        str(out_dir),
    ]
    if not has_crc:
        args.append("--no-crc")
    if impl_header:
        args.append("--impl-header")
    if module_path:
        args += ["--module-path", module_path]

    env = os.environ.copy()
    # Prefer unbuffered for logging
    env["PYTHONUNBUFFERED"] = "1"

    print("[info] invoking GR generator:")
    print(" ", " ".join(args))
    subprocess.check_call(args, env=env, cwd=str(Path.cwd()))

    # Discover the emitted file by pattern
    # The generator names files like: tx_sf{sf}_bw{bw}_cr{cr}_crc{0/1}_impl{0/1}_ldro{ldro}_pay{len}.cf32
    candidates = sorted(out_dir.glob(f"tx_sf{sf}_bw{bw}_cr{cr}_crc*_impl*_ldro{ldro}_pay*.cf32"))
    if not candidates:
        raise RuntimeError("No generated .cf32 found in output directory")
    vec_path = candidates[-1]
    meta_path = vec_path.with_suffix(".json")
    meta = {}
    if meta_path.exists():
        try:
            meta = json.loads(meta_path.read_text())
        except Exception:
            meta = {}
    return vec_path, meta


def parse_complex_list(spec: Optional[str]) -> List[complex]:
    if not spec:
        return []
    parts = []
    for token in spec.split(","):
        token = token.strip()
        if not token:
            continue
        # Forms: a+bj or a+bi or just a
        try:
            c = complex(token.replace("i", "j"))
        except Exception:
            raise ValueError(f"Bad complex tap '{token}', use a+bj")
        parts.append(c)
    return parts


def main() -> None:
    ap = argparse.ArgumentParser(description="Generate a hard LoRa CF32 vector with impairments")
    base = ap.add_mutually_exclusive_group(required=True)
    base.add_argument("--input", type=Path, help="Existing clean CF32 to impair")
    base.add_argument("--emit-with-gr", action="store_true", help="Use GNU Radio generator to create a clean frame")

    # Parameters for GR emission (ignored when --input is used)
    ap.add_argument("--sf", type=int, default=7)
    ap.add_argument("--bw", type=int, default=125000)
    ap.add_argument("--samp-rate", type=int, default=500000)
    ap.add_argument("--cr", type=int, default=2)
    ap.add_argument("--payload", type=str, default="VERY_HARD_VECTOR_TEST")
    ap.add_argument("--no-crc", action="store_true")
    ap.add_argument("--impl-header", action="store_true")
    ap.add_argument("--ldro", type=int, default=2, choices=[0, 1, 2])
    ap.add_argument("--module-path", type=str, default=None, help="Extra path to locate gnuradio.lora_sdr (build/python)")
    ap.add_argument("--gr-out-dir", type=Path, default=Path("vectors/generated"))

    # Impairments
    ap.add_argument("--snr-db", type=float, default=2.0, help="Target post-impairment SNR in dB (inf for none)")
    ap.add_argument("--cfo-frac", type=float, default=0.0025, help="CFO as fraction of sample rate (e.g., 0.002)")
    ap.add_argument("--clk-ppm", type=float, default=30.0, help="Clock drift in ppm (resampling)")
    ap.add_argument("--time-offset", type=int, default=20000, help="Noise-only samples to prepend")
    ap.add_argument("--iq-gain-db", type=float, default=0.8, help="IQ gain mismatch in dB (I channel)")
    ap.add_argument("--iq-phase-deg", type=float, default=3.0, help="IQ phase mismatch in degrees")
    ap.add_argument("--dc", type=str, default="0+0j", help="DC offset as complex a+bj")
    ap.add_argument(
        "--taps",
        type=str,
        default="0.9+0j,0.3-0.1j,0.2+0.05j",
        help="Comma-separated complex multipath taps a+bj",
    )

    # Output
    ap.add_argument("--out", type=Path, default=Path("vectors/hard/hard_vector.cf32"))
    ap.add_argument("--meta", type=Path, default=None, help="Write metadata JSON (defaults next to output)")

    args = ap.parse_args()

    if args.emit_with_gr:
        payload_bytes = args.payload.encode("utf-8") if not args.payload.startswith("0x") else bytes.fromhex(args.payload[2:])
        base_path, base_meta = run_gr_generate(
            out_dir=args.gr_out_dir,
            sf=args.sf,
            bw=args.bw,
            samp_rate=args.samp_rate,
            cr=args.cr,
            payload=payload_bytes,
            has_crc=not args.no_crc,
            impl_header=args.impl_header,
            ldro=args.ldro,
            module_path=args.module_path,
        )
    else:
        if not args.input.exists():
            raise FileNotFoundError(args.input)
        base_path = args.input.resolve()
        base_meta = {}

    iq = _load_cf32(base_path)

    # Apply impairments in a realistic order: DC/IQ -> multipath -> CFO -> clock drift -> time offset -> AWGN
    dc = complex(args.dc.replace("i", "j"))
    taps = parse_complex_list(args.taps)

    iq2 = impair_dc(iq, dc.real, dc.imag)
    iq2 = impair_iq_imbalance(iq2, args.iq_gain_db, args.iq_phase_deg)
    iq2 = impair_multipath(iq2, taps)
    iq2 = impair_cfo(iq2, args.cfo_frac)
    iq2 = impair_clock_drift(iq2, args.clk_ppm)
    iq2 = impair_time_offset(iq2, args.time_offset, args.snr_db)
    iq2 = impair_awgn(iq2, args.snr_db)

    out_path = args.out.expanduser().resolve()
    _save_cf32(out_path, iq2)

    meta = {
        "source": str(base_path),
        "base_meta": base_meta,
        "impairments": {
            "snr_db": args.snr_db,
            "cfo_frac": args.cfo_frac,
            "clk_ppm": args.clk_ppm,
            "time_offset": args.time_offset,
            "iq_gain_db": args.iq_gain_db,
            "iq_phase_deg": args.iq_phase_deg,
            "dc": str(dc),
            "taps": [str(t) for t in taps],
        },
        "output": str(out_path),
        "samples": int(iq2.size),
    }
    meta_path = args.meta or out_path.with_suffix(".json")
    meta_path.parent.mkdir(parents=True, exist_ok=True)
    meta_path.write_text(json.dumps(meta, indent=2))
    print(f"[ok] wrote hard vector -> {out_path} ({iq2.size} samples)")
    print(f"[ok] wrote metadata -> {meta_path}")


if __name__ == "__main__":
    main()
