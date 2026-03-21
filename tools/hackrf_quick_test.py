#!/usr/bin/env python3
"""
Quick HackRF OTA test: capture → decode with host + GNU Radio → compare.

Usage:
    # With live capture
    python tools/hackrf_quick_test.py --capture --sf 7 --bw 125000

    # With existing file
    python tools/hackrf_quick_test.py --iq capture.raw --sf 7 --bw 125000

    # Full comparison against GNU Radio
    python tools/hackrf_quick_test.py --iq capture.cf32 --sf 7 --bw 125000 --compare-gnuradio
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional

import numpy as np

REPO_ROOT = Path(__file__).resolve().parent.parent
HOST_BINARY = REPO_ROOT / "build" / "host_sim" / "lora_replay"
GR_DECODE_SCRIPT = REPO_ROOT / "tools" / "gr_decode_capture.py"
GR_CONDA_ENV = "gr310"
LORA_SDR_PY_PATH = REPO_ROOT / "gr_lora_sdr" / "install" / "lib" / "python3.12" / "site-packages"
LORA_SDR_LIB_PATH = REPO_ROOT / "gr_lora_sdr" / "install" / "lib"


def run_hackrf_capture(
    output_path: Path,
    freq_hz: int = 868_100_000,
    sample_rate: int = 2_000_000,
    duration_s: float = 5.0,
    lna_gain: int = 32,
    vga_gain: int = 20,
) -> bool:
    """Capture IQ samples using hackrf_transfer."""
    num_samples = int(sample_rate * duration_s)
    cmd = [
        "hackrf_transfer",
        "-r", str(output_path),
        "-f", str(freq_hz),
        "-s", str(sample_rate),
        "-l", str(lna_gain),
        "-g", str(vga_gain),
        "-n", str(num_samples),
    ]
    print(f"[HackRF] Capturing {duration_s}s @ {freq_hz/1e6:.3f} MHz...")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=duration_s + 10)
        if result.returncode != 0:
            print(f"[HackRF] Error: {result.stderr}")
            return False
        print(f"[HackRF] Captured to {output_path}")
        return True
    except FileNotFoundError:
        print("[HackRF] hackrf_transfer not found - is HackRF tools installed?")
        return False
    except subprocess.TimeoutExpired:
        print("[HackRF] Capture timed out")
        return False


def hackrf_raw_to_cf32(raw_path: Path, cf32_path: Path) -> np.ndarray:
    """Convert HackRF int8 IQ to complex float32."""
    raw = np.fromfile(raw_path, dtype=np.int8)
    iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
    iq /= 128.0  # Normalize to [-1, 1]
    iq.astype(np.complex64).tofile(cf32_path)
    return iq


def load_cf32(path: Path) -> np.ndarray:
    """Load CF32 file."""
    return np.fromfile(path, dtype=np.complex64)


def analyze_spectrum(iq: np.ndarray, sample_rate: int = 2_000_000) -> dict:
    """Quick spectral analysis."""
    power_db = 10 * np.log10(np.mean(np.abs(iq) ** 2) + 1e-10)
    peak_db = 10 * np.log10(np.max(np.abs(iq) ** 2) + 1e-10)
    
    # FFT to find dominant frequency offset
    fft = np.fft.fftshift(np.fft.fft(iq[:min(len(iq), 65536)]))
    fft_mag = np.abs(fft)
    peak_bin = np.argmax(fft_mag)
    freq_offset = (peak_bin - len(fft) // 2) * sample_rate / len(fft)
    
    return {
        "samples": len(iq),
        "duration_s": len(iq) / sample_rate,
        "mean_power_db": float(power_db),
        "peak_power_db": float(peak_db),
        "est_freq_offset_hz": float(freq_offset),
    }


def decode_with_host(
    cf32_path: Path,
    metadata_path: Path,
    output_dir: Path,
) -> Optional[bytes]:
    """Decode using the standalone host_sim decoder."""
    if not HOST_BINARY.exists():
        print(f"[Host] Binary not found: {HOST_BINARY}")
        return None
    
    payload_file = output_dir / "host_payload.bin"
    summary_file = output_dir / "host_summary.json"
    
    cmd = [
        str(HOST_BINARY),
        "--iq", str(cf32_path),
        "--metadata", str(metadata_path),
        "--dump-payload", str(payload_file),
        "--summary-json", str(summary_file),
        "--ignore-ref-mismatch",
    ]
    
    print("[Host] Decoding...")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        if payload_file.exists() and payload_file.stat().st_size > 0:
            payload = payload_file.read_bytes()
            print(f"[Host] Decoded {len(payload)} bytes")
            return payload
        else:
            print(f"[Host] No payload decoded")
            if result.stderr:
                print(f"[Host] stderr: {result.stderr[:500]}")
            return None
    except Exception as e:
        print(f"[Host] Error: {e}")
        return None


def decode_with_gnuradio(
    cf32_path: Path,
    metadata_path: Path,
    output_dir: Path,
) -> Optional[bytes]:
    """Decode using GNU Radio gr-lora_sdr."""
    if not GR_DECODE_SCRIPT.exists():
        print(f"[GR] Script not found: {GR_DECODE_SCRIPT}")
        return None
    
    payload_file = output_dir / "gr_payload.bin"
    
    env = os.environ.copy()
    env["PYTHONPATH"] = f"{LORA_SDR_PY_PATH}:{env.get('PYTHONPATH', '')}"
    env["LD_LIBRARY_PATH"] = f"{LORA_SDR_LIB_PATH}:{env.get('LD_LIBRARY_PATH', '')}"
    
    cmd = [
        "conda", "run", "-n", GR_CONDA_ENV, "--no-capture-output",
        "python3", str(GR_DECODE_SCRIPT),
        "--input", str(cf32_path),
        "--metadata", str(metadata_path),
        "--payload-out", str(payload_file),
    ]
    
    print("[GR] Decoding with GNU Radio...")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120, env=env)
        if payload_file.exists() and payload_file.stat().st_size > 0:
            payload = payload_file.read_bytes()
            print(f"[GR] Decoded {len(payload)} bytes")
            return payload
        else:
            print(f"[GR] No payload decoded")
            if result.stderr:
                print(f"[GR] stderr: {result.stderr[:500]}")
            return None
    except Exception as e:
        print(f"[GR] Error: {e}")
        return None


def compare_payloads(host_payload: Optional[bytes], gr_payload: Optional[bytes]) -> dict:
    """Compare decoded payloads."""
    result = {
        "host_decoded": host_payload is not None,
        "gr_decoded": gr_payload is not None,
        "match": False,
        "host_len": len(host_payload) if host_payload else 0,
        "gr_len": len(gr_payload) if gr_payload else 0,
    }
    
    if host_payload and gr_payload:
        result["match"] = host_payload == gr_payload
        if not result["match"]:
            # Find first difference
            min_len = min(len(host_payload), len(gr_payload))
            for i in range(min_len):
                if host_payload[i] != gr_payload[i]:
                    result["first_diff_byte"] = i
                    break
            else:
                result["first_diff_byte"] = min_len  # Length difference
    
    return result


def print_summary(spectrum: dict, comparison: dict):
    """Print test summary."""
    print("\n" + "=" * 60)
    print("                    TEST SUMMARY")
    print("=" * 60)
    
    print(f"\n📡 Signal Analysis:")
    print(f"   Samples:        {spectrum['samples']:,}")
    print(f"   Duration:       {spectrum['duration_s']:.2f} s")
    print(f"   Mean Power:     {spectrum['mean_power_db']:.1f} dB")
    print(f"   Peak Power:     {spectrum['peak_power_db']:.1f} dB")
    print(f"   Freq Offset:    {spectrum['est_freq_offset_hz']:.0f} Hz")
    
    print(f"\n🔍 Decode Results:")
    host_status = "✅" if comparison["host_decoded"] else "❌"
    gr_status = "✅" if comparison["gr_decoded"] else "❌"
    print(f"   Host Decoder:   {host_status} ({comparison['host_len']} bytes)")
    print(f"   GNU Radio:      {gr_status} ({comparison['gr_len']} bytes)")
    
    if comparison["host_decoded"] and comparison["gr_decoded"]:
        match_status = "✅ MATCH" if comparison["match"] else "❌ MISMATCH"
        print(f"\n🎯 Comparison:     {match_status}")
        if not comparison["match"] and "first_diff_byte" in comparison:
            print(f"   First diff at byte {comparison['first_diff_byte']}")
    
    print("=" * 60)


def main():
    parser = argparse.ArgumentParser(description="Quick HackRF OTA test")
    parser.add_argument("--capture", action="store_true", help="Capture new IQ from HackRF")
    parser.add_argument("--iq", type=Path, help="Path to existing IQ file (.raw or .cf32)")
    parser.add_argument("--freq", type=float, default=868.1, help="Frequency in MHz")
    parser.add_argument("--sf", type=int, default=7, help="Spreading Factor (5-12)")
    parser.add_argument("--bw", type=int, default=125000, help="Bandwidth in Hz")
    parser.add_argument("--cr", type=int, default=1, help="Coding Rate (1-4 for 4/5-4/8)")
    parser.add_argument("--duration", type=float, default=5.0, help="Capture duration in seconds")
    parser.add_argument("--sample-rate", type=int, default=2_000_000, help="Sample rate")
    parser.add_argument("--compare-gnuradio", action="store_true", help="Also decode with GNU Radio")
    parser.add_argument("--output-dir", type=Path, default=None, help="Output directory")
    
    args = parser.parse_args()
    
    if not args.capture and not args.iq:
        parser.error("Specify --capture or --iq <file>")
    
    # Setup output directory
    if args.output_dir:
        output_dir = args.output_dir
        output_dir.mkdir(parents=True, exist_ok=True)
    else:
        output_dir = Path(tempfile.mkdtemp(prefix="hackrf_test_"))
    
    print(f"Output directory: {output_dir}")
    
    # Get IQ samples
    if args.capture:
        raw_path = output_dir / "capture.raw"
        if not run_hackrf_capture(
            raw_path,
            freq_hz=int(args.freq * 1e6),
            sample_rate=args.sample_rate,
            duration_s=args.duration,
        ):
            sys.exit(1)
        cf32_path = output_dir / "capture.cf32"
        iq = hackrf_raw_to_cf32(raw_path, cf32_path)
    else:
        iq_path = args.iq
        if not iq_path.exists():
            print(f"File not found: {iq_path}")
            sys.exit(1)
        
        if iq_path.suffix == ".raw":
            cf32_path = output_dir / "capture.cf32"
            iq = hackrf_raw_to_cf32(iq_path, cf32_path)
        else:
            cf32_path = iq_path
            iq = load_cf32(cf32_path)
    
    print(f"Loaded {len(iq):,} samples ({len(iq)/args.sample_rate:.2f}s)")
    
    # Analyze spectrum
    spectrum = analyze_spectrum(iq, args.sample_rate)
    
    # Create metadata
    metadata = {
        "sf": args.sf,
        "bw": args.bw,
        "cr": args.cr,
        "crc_enabled": True,
        "implicit_header": False,
        "sample_rate": args.sample_rate,
    }
    metadata_path = output_dir / "metadata.json"
    with open(metadata_path, "w") as f:
        json.dump(metadata, f, indent=2)
    
    # Decode with host
    host_payload = decode_with_host(cf32_path, metadata_path, output_dir)
    
    # Optionally decode with GNU Radio
    gr_payload = None
    if args.compare_gnuradio:
        gr_payload = decode_with_gnuradio(cf32_path, metadata_path, output_dir)
    
    # Compare
    comparison = compare_payloads(host_payload, gr_payload)
    
    # Save results
    results = {
        "spectrum": spectrum,
        "metadata": metadata,
        "comparison": comparison,
    }
    results_path = output_dir / "results.json"
    with open(results_path, "w") as f:
        json.dump(results, f, indent=2)
    
    # Print summary
    print_summary(spectrum, comparison)
    
    if comparison["host_decoded"] and host_payload:
        print(f"\n📦 Payload (hex): {host_payload[:32].hex()}" + 
              ("..." if len(host_payload) > 32 else ""))
        try:
            print(f"📦 Payload (text): {host_payload[:64].decode('utf-8', errors='replace')}")
        except:
            pass
    
    print(f"\n📁 Results saved to: {results_path}")
    
    # Return exit code based on success
    if comparison["host_decoded"]:
        if args.compare_gnuradio:
            sys.exit(0 if comparison["match"] else 1)
        sys.exit(0)
    sys.exit(1)


if __name__ == "__main__":
    main()
