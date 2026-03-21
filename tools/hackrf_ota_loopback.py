#!/usr/bin/env python3
"""
HackRF OTA Loopback Test - TX then RX with single HackRF.

This test:
1. Takes a known-good CF32 LoRa waveform
2. Transmits it via HackRF (into attenuated dummy load)
3. Captures while transmitting using cable loopback
4. Decodes the captured signal
5. Compares with original

For true loopback, connect:
  HackRF TX → 50dB attenuation → splitter → HackRF RX
  (requires 2x HackRF or a circulator)

For single HackRF "sequential" test:
  1. TX into dummy load (validates TX chain)
  2. RX ambient (validates RX chain)
  3. Compare decoder output on known files
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parent.parent
HOST_BINARY = REPO_ROOT / "build" / "host_sim" / "lora_replay"
GR_DECODE_SCRIPT = REPO_ROOT / "tools" / "gr_decode_capture.py"


def cf32_to_hackrf_int8(cf32_path: Path, int8_path: Path) -> int:
    """Convert CF32 (complex float) to HackRF int8 format."""
    iq = np.fromfile(cf32_path, dtype=np.complex64)
    
    # Normalize to [-1, 1] range
    max_val = np.max(np.abs(iq))
    if max_val > 0:
        iq = iq / max_val * 0.9  # Leave some headroom
    
    # Convert to interleaved int8
    int8_data = np.zeros(len(iq) * 2, dtype=np.int8)
    int8_data[0::2] = (iq.real * 127).astype(np.int8)
    int8_data[1::2] = (iq.imag * 127).astype(np.int8)
    
    int8_data.tofile(int8_path)
    return len(iq)


def hackrf_int8_to_cf32(int8_path: Path, cf32_path: Path) -> np.ndarray:
    """Convert HackRF int8 to CF32."""
    raw = np.fromfile(int8_path, dtype=np.int8)
    iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
    iq /= 128.0
    iq.astype(np.complex64).tofile(cf32_path)
    return iq


def hackrf_transmit(
    int8_path: Path,
    freq_hz: int = 868_100_000,
    sample_rate: int = 2_000_000,
    tx_gain: int = 0,  # Low power for testing
    amp_enable: bool = False,
) -> bool:
    """Transmit IQ file via HackRF."""
    cmd = [
        "hackrf_transfer",
        "-t", str(int8_path),
        "-f", str(freq_hz),
        "-s", str(sample_rate),
        "-x", str(tx_gain),
    ]
    if amp_enable:
        cmd.append("-a")
        cmd.append("1")
    
    print(f"[TX] Transmitting @ {freq_hz/1e6:.3f} MHz, gain={tx_gain}dB...")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            print(f"[TX] Error: {result.stderr}")
            return False
        print(f"[TX] Transmission complete")
        return True
    except subprocess.TimeoutExpired:
        print("[TX] Transmission timed out")
        return False
    except FileNotFoundError:
        print("[TX] hackrf_transfer not found")
        return False


def hackrf_receive(
    output_path: Path,
    freq_hz: int = 868_100_000,
    sample_rate: int = 2_000_000,
    duration_s: float = 5.0,
    lna_gain: int = 32,
    vga_gain: int = 20,
) -> bool:
    """Receive IQ samples via HackRF."""
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
    
    print(f"[RX] Receiving {duration_s}s @ {freq_hz/1e6:.3f} MHz...")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=duration_s + 10)
        if result.returncode != 0:
            print(f"[RX] Error: {result.stderr}")
            return False
        print(f"[RX] Capture complete")
        return True
    except subprocess.TimeoutExpired:
        print("[RX] Capture timed out")
        return False


def decode_with_host(cf32_path: Path, metadata: dict, output_dir: Path) -> dict:
    """Decode using host decoder."""
    if not HOST_BINARY.exists():
        return {"status": "error", "message": "Host binary not found"}
    
    metadata_path = output_dir / "metadata.json"
    with open(metadata_path, "w") as f:
        json.dump(metadata, f)
    
    payload_path = output_dir / "host_payload.bin"
    
    cmd = [
        str(HOST_BINARY),
        "--iq", str(cf32_path),
        "--metadata", str(metadata_path),
        "--dump-payload", str(payload_path),
        "--ignore-ref-mismatch",
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        if payload_path.exists() and payload_path.stat().st_size > 0:
            payload = payload_path.read_bytes()
            return {
                "status": "success",
                "payload_len": len(payload),
                "payload_hex": payload[:32].hex(),
            }
        return {"status": "no_packet", "stderr": result.stderr[:200]}
    except Exception as e:
        return {"status": "error", "message": str(e)}


def analyze_signal(iq: np.ndarray, sample_rate: int) -> dict:
    """Analyze captured signal."""
    power_db = 10 * np.log10(np.mean(np.abs(iq) ** 2) + 1e-10)
    peak_db = 10 * np.log10(np.max(np.abs(iq) ** 2) + 1e-10)
    noise_floor = 10 * np.log10(np.percentile(np.abs(iq) ** 2, 10) + 1e-10)
    
    return {
        "samples": len(iq),
        "duration_s": len(iq) / sample_rate,
        "mean_power_db": float(power_db),
        "peak_power_db": float(peak_db),
        "noise_floor_db": float(noise_floor),
        "snr_db": float(peak_db - noise_floor),
    }


def run_ota_test(
    cf32_path: Path,
    metadata_path: Path,
    output_dir: Path,
    freq_hz: int = 868_100_000,
    sample_rate: int = 2_000_000,
    tx_gain: int = 0,
    mode: str = "tx_only",  # tx_only, rx_only, sequential
) -> dict:
    """Run OTA test."""
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Load metadata
    with open(metadata_path) as f:
        metadata = json.load(f)
    
    results = {
        "source_file": str(cf32_path),
        "metadata": metadata,
        "freq_hz": freq_hz,
        "sample_rate": sample_rate,
        "mode": mode,
    }
    
    if mode in ("tx_only", "sequential"):
        # Convert CF32 to HackRF format
        int8_path = output_dir / "tx_signal.raw"
        num_samples = cf32_to_hackrf_int8(cf32_path, int8_path)
        print(f"[Prep] Converted {num_samples:,} samples to HackRF format")
        
        # Transmit
        tx_ok = hackrf_transmit(int8_path, freq_hz, sample_rate, tx_gain)
        results["tx_success"] = tx_ok
        
        if not tx_ok:
            return results
    
    if mode in ("rx_only", "sequential"):
        # Small delay between TX and RX
        if mode == "sequential":
            time.sleep(0.5)
        
        # Receive
        rx_raw_path = output_dir / "rx_capture.raw"
        rx_duration = metadata.get("expected_duration_s", 5.0)
        rx_ok = hackrf_receive(rx_raw_path, freq_hz, sample_rate, rx_duration)
        results["rx_success"] = rx_ok
        
        if rx_ok and rx_raw_path.exists():
            # Convert to CF32
            rx_cf32_path = output_dir / "rx_capture.cf32"
            iq = hackrf_int8_to_cf32(rx_raw_path, rx_cf32_path)
            
            # Analyze
            results["rx_analysis"] = analyze_signal(iq, sample_rate)
            
            # Decode
            results["rx_decode"] = decode_with_host(rx_cf32_path, metadata, output_dir)
    
    # Also decode original for comparison
    results["original_decode"] = decode_with_host(cf32_path, metadata, output_dir)
    
    return results


def main():
    parser = argparse.ArgumentParser(description="HackRF OTA loopback test")
    parser.add_argument("--iq", type=Path, required=True, help="Source CF32 file")
    parser.add_argument("--metadata", type=Path, required=True, help="Metadata JSON")
    parser.add_argument("--output-dir", type=Path, default=Path("build/ota_test"))
    parser.add_argument("--freq", type=float, default=868.1, help="Frequency in MHz")
    parser.add_argument("--tx-gain", type=int, default=0, help="TX gain (0-47)")
    parser.add_argument("--mode", choices=["tx_only", "rx_only", "sequential"], 
                        default="tx_only", help="Test mode")
    
    args = parser.parse_args()
    
    if not args.iq.exists():
        print(f"Error: IQ file not found: {args.iq}")
        sys.exit(1)
    
    if not args.metadata.exists():
        print(f"Error: Metadata file not found: {args.metadata}")
        sys.exit(1)
    
    print("=" * 60)
    print("         HackRF OTA Loopback Test")
    print("=" * 60)
    print(f"Source:    {args.iq}")
    print(f"Mode:      {args.mode}")
    print(f"Frequency: {args.freq} MHz")
    print(f"TX Gain:   {args.tx_gain} dB")
    print("=" * 60)
    
    results = run_ota_test(
        args.iq,
        args.metadata,
        args.output_dir,
        freq_hz=int(args.freq * 1e6),
        tx_gain=args.tx_gain,
        mode=args.mode,
    )
    
    # Save results
    results_path = args.output_dir / "ota_results.json"
    with open(results_path, "w") as f:
        json.dump(results, f, indent=2)
    
    # Print summary
    print("\n" + "=" * 60)
    print("                    RESULTS")
    print("=" * 60)
    
    if "tx_success" in results:
        tx_status = "✅" if results["tx_success"] else "❌"
        print(f"TX: {tx_status}")
    
    if "rx_success" in results:
        rx_status = "✅" if results["rx_success"] else "❌"
        print(f"RX: {rx_status}")
        
        if "rx_analysis" in results:
            ana = results["rx_analysis"]
            print(f"   Power: {ana['mean_power_db']:.1f} dB")
            print(f"   SNR:   {ana['snr_db']:.1f} dB")
    
    if "original_decode" in results:
        orig = results["original_decode"]
        print(f"\nOriginal decode: {orig['status']}")
        if orig["status"] == "success":
            print(f"   Payload: {orig['payload_len']} bytes")
    
    if "rx_decode" in results:
        rxd = results["rx_decode"]
        print(f"\nRX decode: {rxd['status']}")
        if rxd["status"] == "success":
            print(f"   Payload: {rxd['payload_len']} bytes")
    
    print("=" * 60)
    print(f"Results saved to: {results_path}")


if __name__ == "__main__":
    main()
