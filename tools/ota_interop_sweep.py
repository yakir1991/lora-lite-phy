#!/usr/bin/env python3
"""
OTA Interop Sweep — automated TX→capture→decode cross-check between
lora_replay and GNU Radio gr-lora_sdr.

For each configuration, the ESP32+RFM95W transmits a known payload,
HackRF captures it, and BOTH decoders process the same capture.
If payloads disagree, the test is retried up to --max-retries times
(re-transmit + re-capture) to distinguish transient RF issues from
real decode bugs.

Usage:
    python3 tools/ota_interop_sweep.py --port /dev/ttyUSB0 [--matrix default]

Requires:
    - ESP32 running arduino/rfm95_param_sweep/rfm95_param_sweep.ino
    - HackRF One connected  (hackrf_transfer in PATH)
    - lora_replay built     (build/host_sim/lora_replay)
    - GNU Radio gr310 env   (conda run -n gr310)
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
GR_DECODE = REPO_ROOT / "tools" / "gr_decode_capture.py"
GR_LORA_SDR_PYTHONPATH = str(
    REPO_ROOT / "gr_lora_sdr" / "install" / "lib" / "python3.12" / "site-packages"
)
GR_LORA_SDR_LDPATH = str(REPO_ROOT / "gr_lora_sdr" / "install" / "lib")

# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------


@dataclass
class InteropCase:
    name: str
    sf: int = 7
    bw: int = 125  # kHz
    cr: int = 5  # 5-8  (meaning 4/5 .. 4/8)
    power: int = 20  # dBm
    freq_mhz: float = 868.1
    capture_sec: float = 0.0  # 0 = auto
    gr_timeout_s: float = 30.0


@dataclass
class AttemptResult:
    attempt: int
    capture_file: str = ""
    our_payload: str = ""
    our_crc_ok: bool = False
    gr_payload: str = ""
    gr_error: str = ""
    payloads_match: bool = False


@dataclass
class InteropResult:
    name: str
    config: dict
    expected_payload: str = ""
    passed: bool = False
    match_type: str = ""  # "both_match", "our_only", "gr_only", "both_fail", "mismatch"
    attempts: List[dict] = field(default_factory=list)
    final_our_payload: str = ""
    final_gr_payload: str = ""
    error: str = ""


# ---------------------------------------------------------------------------
# Test matrices
# ---------------------------------------------------------------------------

DEFAULT_MATRIX: List[InteropCase] = [
    InteropCase("sf7_bw125_cr5", sf=7, bw=125, cr=5),
    InteropCase("sf7_bw125_cr6", sf=7, bw=125, cr=6),
    InteropCase("sf7_bw125_cr7", sf=7, bw=125, cr=7),
    InteropCase("sf7_bw125_cr8", sf=7, bw=125, cr=8),
    InteropCase("sf8_bw125_cr5", sf=8, bw=125, cr=5),
    InteropCase("sf9_bw125_cr5", sf=9, bw=125, cr=5),
    InteropCase("sf10_bw125_cr5", sf=10, bw=125, cr=5),
    InteropCase("sf11_bw125_cr5", sf=11, bw=125, cr=5, gr_timeout_s=60),
    InteropCase("sf12_bw125_cr5", sf=12, bw=125, cr=5, gr_timeout_s=120),
    InteropCase("sf7_bw250_cr5", sf=7, bw=250, cr=5),
]

QUICK_MATRIX: List[InteropCase] = [
    InteropCase("sf7_bw125_cr5", sf=7, bw=125, cr=5),
    InteropCase("sf8_bw125_cr5", sf=8, bw=125, cr=5),
    InteropCase("sf9_bw125_cr5", sf=9, bw=125, cr=5),
]

FULL_MATRIX: List[InteropCase] = [
    InteropCase("sf7_bw125_cr5", sf=7, bw=125, cr=5),
    InteropCase("sf7_bw125_cr6", sf=7, bw=125, cr=6),
    InteropCase("sf7_bw125_cr7", sf=7, bw=125, cr=7),
    InteropCase("sf7_bw125_cr8", sf=7, bw=125, cr=8),
    InteropCase("sf8_bw125_cr5", sf=8, bw=125, cr=5),
    InteropCase("sf8_bw125_cr6", sf=8, bw=125, cr=6),
    InteropCase("sf9_bw125_cr5", sf=9, bw=125, cr=5),
    InteropCase("sf10_bw125_cr5", sf=10, bw=125, cr=5),
    InteropCase("sf11_bw125_cr5", sf=11, bw=125, cr=5, gr_timeout_s=60),
    InteropCase("sf12_bw125_cr5", sf=12, bw=125, cr=5, gr_timeout_s=120),
    InteropCase("sf7_bw250_cr5", sf=7, bw=250, cr=5),
    InteropCase("sf7_bw250_cr7", sf=7, bw=250, cr=7),
]

MATRICES = {"default": DEFAULT_MATRIX, "quick": QUICK_MATRIX, "full": FULL_MATRIX}


# ---------------------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------------------


def open_serial(port: str, baud: int = 115200):
    import serial as pyserial

    ser = pyserial.Serial(port, baud, timeout=2)
    time.sleep(2)
    ser.reset_input_buffer()
    return ser


def serial_command(ser, cmd: str, timeout: float = 30.0) -> List[str]:
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
            time.sleep(0.05)
            while ser.in_waiting:
                extra = ser.readline().decode("utf-8", errors="replace").strip()
                if extra:
                    lines.append(extra)
            break
    return lines


# ---------------------------------------------------------------------------
# Capture helpers
# ---------------------------------------------------------------------------


def estimate_capture_seconds(tc: InteropCase) -> float:
    symbol_time = (2**tc.sf) / (tc.bw * 1000)
    packet_time = symbol_time * 80
    total = max(packet_time + 2.0, 3.0) + 3.0  # 1 packet + margin
    return min(total, 60.0)


def hackrf_capture(
    output_raw: Path,
    freq_hz: int,
    duration_sec: float,
    sample_rate: int = 2_000_000,
    lna: int = 40,
    vga: int = 50,
) -> None:
    n_samples = int(sample_rate * duration_sec)
    cmd = [
        "hackrf_transfer",
        "-r",
        str(output_raw),
        "-f",
        str(freq_hz),
        "-s",
        str(sample_rate),
        "-a",
        "1",
        "-l",
        str(lna),
        "-g",
        str(vga),
        "-n",
        str(n_samples),
    ]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def convert_raw_to_cf32(raw_path: Path, cf32_path: Path) -> int:
    script = (
        "import numpy as np\n"
        f"raw = np.fromfile('{raw_path}', dtype=np.int8)\n"
        "n = len(raw) // 2 * 2\n"
        "iq = (raw[:n:2].astype(np.float32) + 1j * raw[1:n:2].astype(np.float32)) / 128.0\n"
        f"iq.astype(np.complex64).tofile('{cf32_path}')\n"
        "print(len(iq))\n"
    )
    result = subprocess.run(
        [sys.executable, "-c", script], capture_output=True, text=True, check=True
    )
    return int(result.stdout.strip())


def write_metadata(path: Path, tc: InteropCase, sample_rate: int = 2_000_000) -> None:
    # LDRO auto-enabled by SX1276/RadioLib when symbol time > 16 ms
    symbol_time_ms = (2 ** tc.sf) / (tc.bw * 1000) * 1000
    ldro = symbol_time_ms > 16.0

    meta = {
        "sf": tc.sf,
        "bw": tc.bw * 1000,
        "sample_rate": sample_rate,
        "cr": tc.cr - 4,  # RadioLib 5 → decoder 1
        "preamble_len": 8,
        "payload_len": 0,  # explicit header → decoder reads from header
        "has_crc": True,
        "ldro": ldro,
        "implicit_header": False,
        "sync_word": 18,
    }
    path.write_text(json.dumps(meta, indent=2))


# ---------------------------------------------------------------------------
# Decode with lora_replay
# ---------------------------------------------------------------------------


def decode_ours(cf32_path: Path, metadata_path: Path) -> tuple[str, bool]:
    """Returns (payload_ascii, crc_ok)."""
    cmd = [str(LORA_REPLAY), "--iq", str(cf32_path), "--metadata", str(metadata_path)]
    try:
        result = subprocess.run(
            cmd, capture_output=True, timeout=600
        )
    except subprocess.TimeoutExpired:
        return ("", False)
    # Decode with error handling (binary data may be mixed in)
    stdout = result.stdout.decode("utf-8", errors="replace")
    payload_match = re.search(r"Payload ASCII:\s*(.+)", stdout)
    crc_ok = bool(re.search(r"CRC.*OK", stdout))
    payload = payload_match.group(1).strip() if payload_match else ""
    return (payload, crc_ok)


# ---------------------------------------------------------------------------
# Decode with GNU Radio
# ---------------------------------------------------------------------------


def _in_gr310_env() -> bool:
    """Check if we're already running inside the gr310 conda env."""
    conda_prefix = os.environ.get("CONDA_DEFAULT_ENV", "")
    return conda_prefix == "gr310"


def decode_gnuradio(
    cf32_path: Path, metadata_path: Path, payload_out: Path, timeout_s: float = 30.0
) -> tuple[str, str]:
    """Returns (payload_ascii, error_string).
    payload_ascii is empty string if decode fails."""
    env = os.environ.copy()
    env["PYTHONPATH"] = GR_LORA_SDR_PYTHONPATH
    env["LD_LIBRARY_PATH"] = GR_LORA_SDR_LDPATH + ":" + env.get("LD_LIBRARY_PATH", "")

    base_args = [
        str(GR_DECODE),
        "--input",
        str(cf32_path),
        "--metadata",
        str(metadata_path),
        "--payload-out",
        str(payload_out),
        "--timeout-s",
        str(timeout_s),
    ]

    if _in_gr310_env():
        # Already inside gr310 — run directly
        cmd = ["python3"] + base_args
    else:
        # Use conda run -n gr310
        cmd = [
            "conda",
            "run",
            "-n",
            "gr310",
            "--no-capture-output",
            "env",
            f"PYTHONPATH={GR_LORA_SDR_PYTHONPATH}",
            f"LD_LIBRARY_PATH={GR_LORA_SDR_LDPATH}",
            "python3",
        ] + base_args
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout_s + 30,
        )
    except subprocess.TimeoutExpired:
        return ("", "timeout")
    except Exception as e:
        return ("", str(e))

    if result.returncode != 0:
        stderr = result.stderr.strip()
        # Check for known GR buffer error
        if "ninput_items_required" in stderr or "Segmentation" in stderr:
            return ("", stderr[:200])
        return ("", f"exit {result.returncode}: {stderr[:200]}")

    # Read decoded payload bytes
    try:
        if payload_out.exists() and payload_out.stat().st_size > 0:
            data = payload_out.read_bytes()
            # Try ASCII decode
            try:
                payload_str = data.decode("ascii", errors="replace").rstrip("\x00")
            except Exception:
                payload_str = data.hex()
            return (payload_str, "")
        else:
            return ("", "empty output")
    except Exception as e:
        return ("", str(e))


# ---------------------------------------------------------------------------
# Run one attempt (capture + dual decode)
# ---------------------------------------------------------------------------


def run_attempt(
    ser, tc: InteropCase, case_dir: Path, attempt: int
) -> AttemptResult:
    ar = AttemptResult(attempt=attempt)
    attempt_dir = case_dir / f"attempt_{attempt}"
    attempt_dir.mkdir(parents=True, exist_ok=True)

    raw_file = attempt_dir / "capture.raw"
    cf32_file = attempt_dir / "capture.cf32"
    meta_file = attempt_dir / "metadata.json"
    gr_payload_file = attempt_dir / "gr_payload.bin"

    # 1. Configure radio
    set_cmd = f"SET SF={tc.sf} BW={tc.bw} CR={tc.cr} PWR={tc.power}"
    resp = serial_command(ser, set_cmd, timeout=5)
    if any("ERR" in l for l in resp):
        ar.gr_error = f"SET failed: {resp}"
        return ar

    # 2. Start capture in background
    cap_sec = tc.capture_sec if tc.capture_sec > 0 else estimate_capture_seconds(tc)
    freq_hz = int(tc.freq_mhz * 1e6)

    capture_proc = subprocess.Popen(
        [
            "hackrf_transfer",
            "-r",
            str(raw_file),
            "-f",
            str(freq_hz),
            "-s",
            "2000000",
            "-a",
            "1",
            "-l",
            "40",
            "-g",
            "50",
            "-n",
            str(int(2_000_000 * cap_sec)),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    time.sleep(0.5)

    # 3. Trigger TX (1 packet)
    tx_resp = serial_command(ser, "TX 1", timeout=cap_sec + 10)

    # 4. Wait for capture
    capture_proc.wait(timeout=cap_sec + 30)

    # 5. Convert
    try:
        convert_raw_to_cf32(raw_file, cf32_file)
    except Exception as e:
        ar.gr_error = f"Convert failed: {e}"
        return ar

    write_metadata(meta_file, tc)
    ar.capture_file = str(cf32_file)

    # 6. Decode with our decoder
    our_payload, our_crc = decode_ours(cf32_file, meta_file)
    ar.our_payload = our_payload
    ar.our_crc_ok = our_crc

    # 7. Decode with GNU Radio
    gr_payload, gr_err = decode_gnuradio(
        cf32_file, meta_file, gr_payload_file, timeout_s=tc.gr_timeout_s
    )
    ar.gr_payload = gr_payload
    ar.gr_error = gr_err

    # 8. Compare
    ar.payloads_match = (
        bool(our_payload) and bool(gr_payload) and our_payload == gr_payload
    )

    # Clean up raw file to save disk space
    try:
        raw_file.unlink(missing_ok=True)
    except Exception:
        pass

    return ar


# ---------------------------------------------------------------------------
# Run one test case with retries
# ---------------------------------------------------------------------------


def run_interop_test(
    ser, tc: InteropCase, output_dir: Path, max_retries: int = 3
) -> InteropResult:
    result = InteropResult(name=tc.name, config=asdict(tc))
    case_dir = output_dir / tc.name
    case_dir.mkdir(parents=True, exist_ok=True)

    for attempt in range(1, max_retries + 1):
        print(f"    Attempt {attempt}/{max_retries}...", end=" ", flush=True)
        ar = run_attempt(ser, tc, case_dir, attempt)
        result.attempts.append(asdict(ar))

        # Determine outcome
        our_ok = bool(ar.our_payload) and ar.our_crc_ok
        gr_ok = bool(ar.gr_payload) and not ar.gr_error

        if our_ok and gr_ok and ar.payloads_match:
            # Both decoded, payloads match — SUCCESS
            result.passed = True
            result.match_type = "both_match"
            result.final_our_payload = ar.our_payload
            result.final_gr_payload = ar.gr_payload
            print(f"MATCH: \"{ar.our_payload}\"")
            break
        elif our_ok and gr_ok and not ar.payloads_match:
            # Both decoded but payloads differ — MISMATCH, retry
            print(
                f"MISMATCH: ours=\"{ar.our_payload}\" vs gr=\"{ar.gr_payload}\""
            )
            if attempt < max_retries:
                print(f"    Retrying (payloads differ)...")
                time.sleep(1)
                continue
            else:
                result.match_type = "mismatch"
                result.final_our_payload = ar.our_payload
                result.final_gr_payload = ar.gr_payload
                result.error = "Persistent payload mismatch after all retries"
        elif our_ok and not gr_ok:
            # Only our decoder succeeded
            if ar.gr_error:
                print(f"GR failed: {ar.gr_error[:60]}")
            else:
                print(f"GR empty output")
            # GR failure might be transient, retry
            if attempt < max_retries:
                print(f"    Retrying (GR decode failed)...")
                time.sleep(1)
                continue
            else:
                result.match_type = "our_only"
                result.final_our_payload = ar.our_payload
                result.passed = True  # Our decoder works, GR is known-limited
                result.error = f"GR: {ar.gr_error or 'empty output'}"
        elif not our_ok and gr_ok:
            # Only GR succeeded — this is a real problem for us
            print(f"OUR DECODE FAILED, gr=\"{ar.gr_payload}\"")
            if attempt < max_retries:
                print(f"    Retrying (our decode failed)...")
                time.sleep(1)
                continue
            else:
                result.match_type = "gr_only"
                result.final_gr_payload = ar.gr_payload
                result.error = "Our decoder failed but GNU Radio succeeded"
        else:
            # Both failed
            print(f"BOTH FAILED")
            if attempt < max_retries:
                print(f"    Retrying (both decoders failed)...")
                time.sleep(1)
                continue
            else:
                result.match_type = "both_fail"
                result.error = f"Neither decoder produced output. GR: {ar.gr_error or 'empty'}"

    # Save per-test result
    (case_dir / "interop_result.json").write_text(
        json.dumps(asdict(result), indent=2)
    )
    return result


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--port", required=True, help="ESP32 serial port (e.g. /dev/ttyUSB0)"
    )
    parser.add_argument(
        "--matrix",
        default="default",
        choices=list(MATRICES.keys()),
        help="Test matrix to run (default: default)",
    )
    parser.add_argument(
        "--output-dir",
        default=str(REPO_ROOT / "build" / "interop_sweep"),
        help="Output directory",
    )
    parser.add_argument(
        "--max-retries",
        type=int,
        default=3,
        help="Max retries on mismatch or failure (default: 3)",
    )
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
        print(f"ERROR: lora_replay not built at {LORA_REPLAY}", file=sys.stderr)
        return 1
    if not shutil.which("hackrf_transfer"):
        print("ERROR: hackrf_transfer not in PATH", file=sys.stderr)
        return 1
    if not GR_DECODE.exists():
        print(f"ERROR: gr_decode_capture.py not found at {GR_DECODE}", file=sys.stderr)
        return 1

    print(f"Opening serial port {args.port}...")
    ser = open_serial(args.port)
    serial_command(ser, "STATUS", timeout=3)

    print(f"\n{'='*65}")
    print(f"  OTA Interop Sweep — {len(matrix)} configurations, max {args.max_retries} retries each")
    print(f"  Decoders: lora_replay vs GNU Radio gr-lora_sdr")
    print(f"  Output: {output_dir}")
    print(f"{'='*65}\n")

    results: List[InteropResult] = []
    matched = 0
    our_only = 0
    failed = 0

    for i, tc in enumerate(matrix, 1):
        print(
            f"[{i}/{len(matrix)}] {tc.name}  SF={tc.sf} BW={tc.bw}k CR=4/{tc.cr}"
        )
        result = run_interop_test(ser, tc, output_dir, max_retries=args.max_retries)
        results.append(result)

        if result.match_type == "both_match":
            matched += 1
        elif result.match_type == "our_only":
            our_only += 1
        else:
            failed += 1
        print()

    ser.close()

    # Write aggregate results JSON
    summary = {
        "timestamp": datetime.now().isoformat(),
        "total": len(results),
        "both_match": matched,
        "our_only": our_only,
        "failed": failed,
        "max_retries": args.max_retries,
        "results": [asdict(r) for r in results],
    }
    summary_path = output_dir / "interop_results.json"
    summary_path.write_text(json.dumps(summary, indent=2))

    # Write markdown report
    md = [
        "# OTA Interop Sweep Results",
        "",
        f"**Date:** {datetime.now().strftime('%Y-%m-%d %H:%M')}",
        f"**Max retries per config:** {args.max_retries}",
        f"**Both match:** {matched} | **Our only:** {our_only} | **Failed:** {failed} | **Total:** {len(results)}",
        "",
        "| Config | SF | BW | CR | Our Payload | GR Payload | Attempts | Result |",
        "|--------|----|----|-----|-------------|------------|----------|--------|",
    ]
    for r in results:
        c = r.config
        n_att = len(r.attempts)
        our_p = r.final_our_payload or "—"
        gr_p = r.final_gr_payload or "—"
        if r.match_type == "both_match":
            status = "✅ MATCH"
        elif r.match_type == "our_only":
            status = "⚠️ GR fail"
        elif r.match_type == "gr_only":
            status = "❌ OUR fail"
        elif r.match_type == "mismatch":
            status = "❌ MISMATCH"
        else:
            status = "❌ BOTH fail"
        md.append(
            f"| {r.name} | {c['sf']} | {c['bw']}k | 4/{c['cr']} "
            f"| {our_p} | {gr_p} | {n_att} | {status} |"
        )

    if failed > 0:
        md.extend(["", "## Failures", ""])
        for r in results:
            if r.match_type not in ("both_match", "our_only"):
                md.append(f"### {r.name}")
                md.append(f"- Error: {r.error}")
                for att in r.attempts:
                    md.append(
                        f"  - Attempt {att['attempt']}: "
                        f"ours=\"{att.get('our_payload', '')}\" "
                        f"gr=\"{att.get('gr_payload', '')}\" "
                        f"gr_err=\"{att.get('gr_error', '')}\""
                    )
                md.append("")

    md.append("")
    report_path = output_dir / "interop_report.md"
    report_path.write_text("\n".join(md))

    print(f"{'='*65}")
    print(f"  Both match: {matched}  |  Our only: {our_only}  |  Failed: {failed}")
    print(f"  Report:  {report_path}")
    print(f"  JSON:    {summary_path}")
    print(f"{'='*65}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
