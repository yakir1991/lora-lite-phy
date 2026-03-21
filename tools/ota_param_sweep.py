#!/usr/bin/env python3
"""
OTA Parameter Sweep — automated TX→capture→decode test.

Drives an ESP32+RFM95W over Serial (rfm95_param_sweep sketch) while
capturing with HackRF, then decodes each capture with lora_replay.

Usage:
    python3 tools/ota_param_sweep.py --port /dev/ttyUSB0 [--matrix default]

Requires:
    - ESP32 running arduino/rfm95_param_sweep/rfm95_param_sweep.ino
    - HackRF One connected  (hackrf_transfer in PATH)
    - lora_replay built     (build/host_sim/lora_replay)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path
from typing import List, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
LORA_REPLAY = REPO_ROOT / "build" / "host_sim" / "lora_replay"

# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

@dataclass
class TestCase:
    name: str
    sf: int = 7
    bw: int = 125       # kHz  (RadioLib uses kHz)
    cr: int = 5          # 5-8  (meaning 4/5 .. 4/8)
    power: int = 10      # dBm
    atten_db: int = 50   # attenuator stack (informational)
    packets: int = 5
    payload: str = ""    # empty = auto "LoRa Test #N"
    freq_mhz: float = 868.1
    capture_sec: float = 0.0  # 0 = auto-compute


@dataclass
class TestResult:
    name: str
    config: dict
    capture_file: str = ""
    packets_sent: int = 0
    packets_decoded: int = 0
    payloads: list = field(default_factory=list)
    decode_stdout: str = ""
    error: str = ""
    passed: bool = False

# ---------------------------------------------------------------------------
# Built-in test matrices
# ---------------------------------------------------------------------------

DEFAULT_MATRIX: List[TestCase] = [
    TestCase("sf7_bw125_cr5",   sf=7,  bw=125, cr=5, atten_db=50),
    TestCase("sf7_bw125_cr6",   sf=7,  bw=125, cr=6, atten_db=50),
    TestCase("sf7_bw125_cr7",   sf=7,  bw=125, cr=7, atten_db=50),
    TestCase("sf7_bw125_cr8",   sf=7,  bw=125, cr=8, atten_db=50),
    TestCase("sf8_bw125_cr5",   sf=8,  bw=125, cr=5, atten_db=50),
    TestCase("sf9_bw125_cr5",   sf=9,  bw=125, cr=5, atten_db=50),
    TestCase("sf10_bw125_cr5",  sf=10, bw=125, cr=5, atten_db=50),
    TestCase("sf12_bw125_cr5",  sf=12, bw=125, cr=5, atten_db=50, packets=2),
    TestCase("sf7_bw250_cr5",   sf=7,  bw=250, cr=5, atten_db=50),
    TestCase("sf7_bw125_lowsnr", sf=7, bw=125, cr=5, atten_db=60),
    TestCase("sf12_bw125_lowsnr", sf=12, bw=125, cr=5, atten_db=60, packets=2),
]

QUICK_MATRIX: List[TestCase] = [
    TestCase("quick_sf7",  sf=7,  bw=125, cr=5, atten_db=50, packets=3),
    TestCase("quick_sf9",  sf=9,  bw=125, cr=5, atten_db=50, packets=2),
    TestCase("quick_sf12", sf=12, bw=125, cr=5, atten_db=50, packets=1),
]

MATRICES = {"default": DEFAULT_MATRIX, "quick": QUICK_MATRIX}

# ---------------------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------------------

def open_serial(port: str, baud: int = 115200):
    """Open serial port. Returns a pyserial Serial object."""
    import serial as pyserial          # pip install pyserial
    ser = pyserial.Serial(port, baud, timeout=2)
    time.sleep(2)                      # wait for Arduino reset
    ser.reset_input_buffer()
    return ser


def serial_command(ser, cmd: str, timeout: float = 30.0) -> List[str]:
    """Send a command and collect response lines until DONE/OK/ERR or timeout."""
    ser.reset_input_buffer()
    ser.write((cmd.strip() + "\n").encode())
    lines: List[str] = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if line:
            lines.append(line)
        if any(line.startswith(k) for k in ("DONE", "OK", "ERR", "CONFIG")):
            # keep reading a bit for trailing output
            time.sleep(0.05)
            while ser.in_waiting:
                extra = ser.readline().decode("utf-8", errors="replace").strip()
                if extra:
                    lines.append(extra)
            break
    return lines


# ---------------------------------------------------------------------------
# HackRF capture
# ---------------------------------------------------------------------------

def estimate_capture_seconds(tc: TestCase) -> float:
    """Rough estimate of how long to capture for N packets."""
    # Airtime per symbol ≈ 2^SF / BW
    symbol_time = (2 ** tc.sf) / (tc.bw * 1000)  # seconds
    # Very rough: preamble(12) + payload(~50 symbols) per packet
    packet_time = symbol_time * 80
    # Inter-packet gap in sweep sketch = intervalMs (default 1 s)
    total = tc.packets * max(packet_time + 1.5, 2.0) + 3.0
    return min(total, 120.0)


def hackrf_capture(output_raw: Path, freq_hz: int, duration_sec: float,
                   sample_rate: int = 2_000_000, lna: int = 32, vga: int = 32) -> None:
    """Run hackrf_transfer to capture IQ."""
    n_samples = int(sample_rate * duration_sec)
    cmd = [
        "hackrf_transfer", "-r", str(output_raw),
        "-f", str(freq_hz),
        "-s", str(sample_rate),
        "-l", str(lna), "-g", str(vga),
        "-n", str(n_samples),
    ]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def convert_raw_to_cf32(raw_path: Path, cf32_path: Path) -> int:
    """Convert HackRF int8 IQ to complex float32. Returns sample count."""
    script = f"""
import numpy as np, sys
raw = np.fromfile("{raw_path}", dtype=np.int8)
n = len(raw) // 2 * 2
iq = (raw[:n:2].astype(np.float32) + 1j * raw[1:n:2].astype(np.float32)) / 128.0
iq.astype(np.complex64).tofile("{cf32_path}")
print(len(iq))
"""
    result = subprocess.run([sys.executable, "-c", script],
                            capture_output=True, text=True, check=True)
    return int(result.stdout.strip())


# ---------------------------------------------------------------------------
# Decode
# ---------------------------------------------------------------------------

def write_metadata(path: Path, tc: TestCase, sample_rate: int = 2_000_000) -> None:
    meta = {
        "sf": tc.sf,
        "bw": tc.bw * 1000,
        "sample_rate": sample_rate,
        "cr": tc.cr - 4,          # internal CR: RadioLib 5 → decoder 1
        "payload_len": 32,
        "preamble_len": 8,
        "implicit_header": False,
        "has_crc": True,
    }
    path.write_text(json.dumps(meta, indent=2))


def decode_capture(cf32_path: Path, metadata_path: Path) -> tuple[int, str]:
    """Run lora_replay, return (packets_decoded, stdout)."""
    cmd = [
        str(LORA_REPLAY),
        "--iq", str(cf32_path),
        "--metadata", str(metadata_path),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    stdout = result.stdout
    # Count decoded payloads (lines matching "Payload ASCII:")
    payloads = re.findall(r"Payload ASCII:\s*(.+)", stdout)
    return len(payloads), stdout


# ---------------------------------------------------------------------------
# Run one test case
# ---------------------------------------------------------------------------

def run_test(ser, tc: TestCase, output_dir: Path) -> TestResult:
    result = TestResult(name=tc.name, config=asdict(tc))
    case_dir = output_dir / tc.name
    case_dir.mkdir(parents=True, exist_ok=True)

    # 1. Configure radio
    set_cmd = f"SET SF={tc.sf} BW={tc.bw} CR={tc.cr} PWR={tc.power}"
    resp = serial_command(ser, set_cmd, timeout=5)
    if any("ERR" in l for l in resp):
        result.error = f"SET failed: {resp}"
        return result

    # 2. Start HackRF capture in background
    raw_file = case_dir / "capture.raw"
    cf32_file = case_dir / "capture.cf32"
    meta_file = case_dir / "metadata.json"
    cap_sec = tc.capture_sec if tc.capture_sec > 0 else estimate_capture_seconds(tc)
    freq_hz = int(tc.freq_mhz * 1e6)

    capture_proc = subprocess.Popen(
        ["hackrf_transfer", "-r", str(raw_file),
         "-f", str(freq_hz), "-s", "2000000",
         "-l", "32", "-g", "32",
         "-n", str(int(2_000_000 * cap_sec))],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    time.sleep(0.5)  # let capture start

    # 3. Trigger TX
    tx_cmd = f"TX {tc.packets}"
    if tc.payload:
        tx_cmd += f" {tc.payload}"
    tx_timeout = cap_sec + 10
    tx_resp = serial_command(ser, tx_cmd, timeout=tx_timeout)
    result.packets_sent = tc.packets

    # 4. Wait for capture to finish
    capture_proc.wait(timeout=cap_sec + 30)

    # 5. Convert and decode
    try:
        n_samples = convert_raw_to_cf32(raw_file, cf32_file)
        result.capture_file = str(cf32_file)
        write_metadata(meta_file, tc)
        n_decoded, stdout = decode_capture(cf32_file, meta_file)
        result.packets_decoded = n_decoded
        result.decode_stdout = stdout
        result.payloads = re.findall(r"Payload ASCII:\s*(.+)", stdout)
        result.passed = n_decoded >= 1
    except Exception as e:
        result.error = str(e)

    # Save per-test result
    (case_dir / "result.json").write_text(json.dumps(asdict(result), indent=2))
    return result


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True, help="ESP32 serial port (e.g. /dev/ttyUSB0)")
    parser.add_argument("--matrix", default="default", choices=list(MATRICES.keys()),
                        help="Test matrix to run (default: default)")
    parser.add_argument("--output-dir", default=str(REPO_ROOT / "build" / "ota_sweep_results"),
                        help="Output directory")
    parser.add_argument("--only", nargs="*", help="Run only named test cases")
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    matrix = MATRICES[args.matrix]
    if args.only:
        wanted = set(args.only)
        matrix = [tc for tc in matrix if tc.name in wanted]

    # Verify prerequisites
    if not LORA_REPLAY.exists():
        print(f"ERROR: lora_replay not found at {LORA_REPLAY}", file=sys.stderr)
        return 1
    if not shutil.which("hackrf_transfer"):
        print("ERROR: hackrf_transfer not in PATH", file=sys.stderr)
        return 1

    print(f"Opening serial port {args.port}...")
    ser = open_serial(args.port)
    # Drain startup messages
    serial_command(ser, "STATUS", timeout=3)

    print(f"\n{'='*60}")
    print(f"  OTA Parameter Sweep — {len(matrix)} test cases")
    print(f"  Output: {output_dir}")
    print(f"{'='*60}\n")

    results: List[TestResult] = []
    passed = 0
    failed = 0

    for i, tc in enumerate(matrix, 1):
        print(f"[{i}/{len(matrix)}] {tc.name}  SF={tc.sf} BW={tc.bw} CR=4/{tc.cr} "
              f"atten={tc.atten_db}dB  packets={tc.packets}")

        if tc.atten_db != (matrix[0].atten_db if i > 1 else tc.atten_db):
            # Different attenuation — prompt user to change attenuators
            input(f"  >>> Change attenuators to {tc.atten_db} dB, then press Enter...")

        result = run_test(ser, tc, output_dir)
        results.append(result)

        status = "PASS" if result.passed else "FAIL"
        if result.passed:
            passed += 1
        else:
            failed += 1

        print(f"  {status}: decoded {result.packets_decoded}/{result.packets_sent}")
        if result.error:
            print(f"  Error: {result.error}")
        print()

    ser.close()

    # Write aggregate results
    summary = {
        "timestamp": datetime.now().isoformat(),
        "total": len(results),
        "passed": passed,
        "failed": failed,
        "results": [asdict(r) for r in results],
    }
    summary_path = output_dir / "sweep_results.json"
    summary_path.write_text(json.dumps(summary, indent=2))

    # Write markdown report
    md_lines = [
        "# OTA Parameter Sweep Results",
        "",
        f"**Date:** {datetime.now().strftime('%Y-%m-%d %H:%M')}",
        f"**Passed:** {passed}/{len(results)}",
        "",
        "| Test | SF | BW | CR | Atten | Sent | Decoded | Status |",
        "|------|----|----|-----|-------|------|---------|--------|",
    ]
    for r in results:
        c = r.config
        status = "PASS" if r.passed else "**FAIL**"
        md_lines.append(
            f"| {r.name} | {c['sf']} | {c['bw']}k | 4/{c['cr']} | {c['atten_db']}dB "
            f"| {r.packets_sent} | {r.packets_decoded} | {status} |"
        )
    md_lines.append("")
    report_path = output_dir / "sweep_report.md"
    report_path.write_text("\n".join(md_lines))

    print(f"{'='*60}")
    print(f"  Results: {passed} passed, {failed} failed out of {len(results)}")
    print(f"  Report:  {report_path}")
    print(f"  JSON:    {summary_path}")
    print(f"{'='*60}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
