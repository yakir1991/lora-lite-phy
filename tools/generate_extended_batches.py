#!/usr/bin/env python3
"""
Generate extended LoRa capture batches for SF5, SF6, SF11, SF12 
with both explicit and implicit header modes.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path
from typing import Dict, Any, List
import json
import secrets
import math

REPO_ROOT = Path(__file__).resolve().parents[1]
GR_EXAMPLE = REPO_ROOT / "gr_lora_sdr" / "examples" / "tx_rx_simulation.py"
PAYLOAD_DIR = REPO_ROOT / "build"
CONDA_ENV = os.environ.get("LORA_CONDA_ENV", "gr310")


def ensure_payload(path: Path, length: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        try:
            if path.stat().st_size == length:
                return
        except OSError:
            pass
    path.write_bytes(secrets.token_bytes(length))


def estimate_packet_symbols(
    *,
    sf: int,
    cr: int,
    payload_len: int,
    preamble_len: int,
    has_crc: bool,
    implicit_header: bool,
    ldro: bool,
) -> float:
    """Estimate the LoRa packet length in symbols (Semtech airtime model)."""
    de = 1 if ldro else 0
    ih = 1 if implicit_header else 0
    crc = 1 if has_crc else 0

    preamble = float(preamble_len) + 4.25

    denom = 4.0 * (sf - 2 * de)
    if denom <= 0:
        denom = 1.0
    numer = 8.0 * payload_len - 4.0 * sf + 28.0 + 16.0 * crc - 20.0 * ih
    payload_sym = 8.0
    if numer > 0:
        payload_sym += math.ceil(numer / denom) * (cr + 4)
    return preamble + payload_sym


def estimate_capture_samples(config: Dict[str, Any], margin: float = 1.15) -> int:
    sf = int(config["sf"])
    bw = float(config["bw"])
    sample_rate = float(config["sample_rate"])
    cr = int(config["cr"])
    payload_len = int(config.get("payload_len", 32))
    preamble_len = int(config.get("preamble_len", 8))
    has_crc = bool(config.get("has_crc", True))
    implicit_header = bool(config.get("implicit_header", False))
    ldro = bool(config.get("ldro", 0))

    symbols = estimate_packet_symbols(
        sf=sf,
        cr=cr,
        payload_len=payload_len,
        preamble_len=preamble_len,
        has_crc=has_crc,
        implicit_header=implicit_header,
        ldro=ldro,
    )
    samples_per_symbol = (2**sf) * (sample_rate / bw)
    return int(math.ceil(symbols * samples_per_symbol * margin))


def run_capture(config: Dict[str, Any], output_dir: Path) -> None:
    payload_path = PAYLOAD_DIR / f"payload_{config['name']}.bin"
    ensure_payload(payload_path, config.get("payload_len", 32))

    capture_samples = estimate_capture_samples(config, margin=config.get("capture_margin", 1.5))
    
    # Add tail guard
    sf = int(config["sf"])
    bw = int(config["bw"])
    sample_rate = int(config["sample_rate"])
    oversample = max(1, sample_rate // max(1, bw))
    samples_per_symbol = (1 << sf) * oversample
    tail_guard_symbols = 3
    capture_samples += tail_guard_symbols * samples_per_symbol + 128

    # TX delay: more for higher SFs to allow receiver to stabilize
    if sf >= 11:
        tx_delay_samples = 4 * samples_per_symbol
    else:
        tx_delay_samples = 2 * samples_per_symbol

    throttle_factor = 10.0
    wall_needed = float(capture_samples) / (sample_rate * throttle_factor)
    autostop_secs = max(0.5, wall_needed * 2.5 + 0.5)

    env = os.environ.copy()
    env.update(
        {
            "PYTHONPATH": str(REPO_ROOT / "gr_lora_sdr" / "install" / "lib" / "python3.12" / "site-packages")
            + ":" + str(REPO_ROOT / "gr_lora_sdr" / "python"),
            "LD_LIBRARY_PATH": str(REPO_ROOT / "gr_lora_sdr" / "install" / "lib"),
            "LORA_OUTPUT_DIR": str(output_dir),
            "LORA_OUTPUT_NAME": config["output_name"],
            "LORA_PAYLOAD_SOURCE": str(payload_path),
            "LORA_AUTOSTOP_SECS": str(autostop_secs),
            "LORA_SF": str(config["sf"]),
            "LORA_BW": str(config["bw"]),
            "LORA_SAMPLE_RATE": str(config["sample_rate"]),
            "LORA_CR": str(config["cr"]),
            "LORA_PAYLOAD_LEN": str(config.get("payload_len", 32)),
            "LORA_PREAMBLE_LEN": str(config.get("preamble_len", 8)),
            "LORA_LDRO": "1" if config.get("ldro") else "0",
            "LORA_SNR_DB": str(config.get("snr_db", 0.0)),
            "LORA_IMPLICIT_HEADER": "1" if config.get("implicit_header") else "0",
            "LORA_HAS_CRC": "1" if config.get("has_crc", True) else "0",
            "LORA_PAYLOAD_REPEAT": "0",
            "LORA_CAPTURE_SAMPLES": str(capture_samples),
            "LORA_TX_DELAY_SAMPLES": str(tx_delay_samples),
        }
    )

    cmd = [
        "conda",
        "run",
        "-n",
        CONDA_ENV,
        "python",
        str(GR_EXAMPLE),
    ]

    print(f"[generate] {config['name']} -> {config['output_name']}")
    result = subprocess.run(cmd, env=env, cwd=str(REPO_ROOT), capture_output=True, text=True)
    if result.returncode != 0:
        print(f"ERROR: {result.stderr}")
        raise RuntimeError(f"Capture generation failed for {config['name']}")

    capture_path = output_dir / config["output_name"]
    if not capture_path.exists():
        raise FileNotFoundError(f"Expected capture not found: {capture_path}")

    size_bytes = capture_path.stat().st_size
    sample_count = size_bytes // 8

    # Ensure exact length
    target_bytes = int(capture_samples) * 8
    if size_bytes != target_bytes:
        if size_bytes > target_bytes:
            with capture_path.open("r+b") as f:
                f.truncate(target_bytes)
        else:
            pad_bytes = target_bytes - size_bytes
            with capture_path.open("ab") as f:
                f.write(b"\x00" * pad_bytes)
        sample_count = int(capture_samples)

    metadata = {
        "sf": config["sf"],
        "bw": config["bw"],
        "sample_rate": config["sample_rate"],
        "cr": config["cr"],
        "payload_len": config.get("payload_len", 32),
        "preamble_len": config.get("preamble_len", 8),
        "implicit_header": bool(config.get("implicit_header")),
        "has_crc": bool(config.get("has_crc", True)),
        "ldro": int(config.get("ldro", 0)),
        "sync_word": 18,
        "capture_file": config["output_name"],
        "sample_count": sample_count,
        "duration_secs": sample_count / config["sample_rate"],
        "snr_db": config.get("snr_db", 0.0),
    }
    metadata_path = capture_path.with_suffix(".json")
    metadata_path.write_text(json.dumps(metadata, indent=2))


def generate_configs() -> List[Dict[str, Any]]:
    """Generate configurations for SF5, SF6, SF11, SF12 with explicit and implicit headers."""
    configs = []
    
    # SF5 configurations
    for profile, snr, implicit in [
        ("baseline", 5.0, False),
        ("low_snr", -5.0, False),
        ("implicithdr_nocrc_baseline", 3.0, True),
        ("implicithdr_nocrc_low_snr", -5.0, True),
    ]:
        configs.append({
            "name": f"sf5_bw125_cr1_short_{profile}",
            "sf": 5,
            "bw": 125000,
            "sample_rate": 500000,
            "cr": 1,
            "payload_len": 16,
            "preamble_len": 8,
            "ldro": 0,
            "snr_db": snr,
            "implicit_header": implicit,
            "has_crc": not implicit,
            "output_name": f"sf5_bw125_cr1_short_{profile}.cf32",
        })

    # SF6 configurations
    for profile, snr, implicit in [
        ("baseline", 5.0, False),
        ("low_snr", -5.0, False),
        ("implicithdr_nocrc_baseline", 3.0, True),
        ("implicithdr_nocrc_low_snr", -5.0, True),
    ]:
        configs.append({
            "name": f"sf6_bw125_cr2_short_{profile}",
            "sf": 6,
            "bw": 125000,
            "sample_rate": 500000,
            "cr": 2,
            "payload_len": 24,
            "preamble_len": 8,
            "ldro": 0,
            "snr_db": snr,
            "implicit_header": implicit,
            "has_crc": not implicit,
            "output_name": f"sf6_bw125_cr2_short_{profile}.cf32",
        })

    # SF11 configurations
    for profile, snr, implicit in [
        ("baseline", 5.0, False),
        ("low_snr", -10.0, False),
        ("implicithdr_nocrc_baseline", 3.0, True),
        ("implicithdr_nocrc_low_snr", -10.0, True),
    ]:
        configs.append({
            "name": f"sf11_bw125_cr1_long_{profile}",
            "sf": 11,
            "bw": 125000,
            "sample_rate": 500000,
            "cr": 1,
            "payload_len": 48,
            "preamble_len": 8,
            "ldro": 1,
            "snr_db": snr,
            "implicit_header": implicit,
            "has_crc": not implicit,
            "output_name": f"sf11_bw125_cr1_long_{profile}.cf32",
        })

    # SF12 configurations
    for profile, snr, implicit in [
        ("baseline", 5.0, False),
        ("low_snr", -12.0, False),
        ("implicithdr_nocrc_baseline", 3.0, True),
        ("implicithdr_nocrc_low_snr", -12.0, True),
    ]:
        configs.append({
            "name": f"sf12_bw125_cr1_long_{profile}",
            "sf": 12,
            "bw": 125000,
            "sample_rate": 500000,
            "cr": 1,
            "payload_len": 64,
            "preamble_len": 8,
            "ldro": 1,
            "snr_db": snr,
            "implicit_header": implicit,
            "has_crc": not implicit,
            "output_name": f"sf12_bw125_cr1_long_{profile}.cf32",
        })

    return configs


def main():
    output_dir = REPO_ROOT / "build" / "receiver_vs_gnuradio_batches" / "extended"
    output_dir.mkdir(parents=True, exist_ok=True)

    configs = generate_configs()
    
    print(f"Generating {len(configs)} captures in {output_dir}")
    
    for i, config in enumerate(configs, 1):
        print(f"[{i}/{len(configs)}] Generating {config['name']}...")
        try:
            run_capture(config, output_dir)
            print(f"  ✓ Success")
        except Exception as e:
            print(f"  ✗ Failed: {e}")
            continue

    print(f"\nGeneration complete. Output in {output_dir}")


if __name__ == "__main__":
    main()
