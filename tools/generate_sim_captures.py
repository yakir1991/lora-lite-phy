#!/usr/bin/env python3
"""
Generate LoRa CF32 captures via gr_lora_sdr/examples/tx_rx_simulation.py
to expand comparison coverage without hardware.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path
from typing import Dict, Any
import json
import secrets
import argparse
import math

REPO_ROOT = Path(__file__).resolve().parents[1]
GR_EXAMPLE = REPO_ROOT / "gr_lora_sdr" / "examples" / "tx_rx_simulation.py"
OUTPUT_DIR = REPO_ROOT / "gr_lora_sdr" / "data" / "generated"
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

    # Preamble is Npreamble + 4.25 symbols
    preamble = float(preamble_len) + 4.25

    # Payload symbol count (explicit header adds its own overhead through IH=0)
    # payloadSymbNb = 8 + max(ceil((8*PL - 4*SF + 28 + 16*CRC - 20*IH) / (4*(SF-2*DE))) * (CR+4), 0)
    denom = 4.0 * (sf - 2 * de)
    if denom <= 0:
        denom = 1.0
    numer = 8.0 * payload_len - 4.0 * sf + 28.0 + 16.0 * crc - 20.0 * ih
    payload_sym = 8.0
    if numer > 0:
        payload_sym += math.ceil(numer / denom) * (cr + 4)
    return preamble + payload_sym


DEFAULT_CAPTURE_MARGIN = 1.3


def estimate_capture_samples(config: Dict[str, Any], margin: float = DEFAULT_CAPTURE_MARGIN) -> int:
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

    capture_samples = config.get("capture_samples")
    if not isinstance(capture_samples, int) or capture_samples <= 0:
        margin = config.get("capture_margin")
        if not isinstance(margin, (int, float)) or margin <= 0:
            margin = DEFAULT_CAPTURE_MARGIN
        capture_samples = estimate_capture_samples(config, margin=float(margin))

    # Add a small tail guard to avoid end-of-stream scheduler errors in GNU Radio
    # blocks (notably frame_sync) which may request slightly more than one symbol
    # worth of samples near the end of the file.
    try:
        sf = int(config["sf"])
        bw = int(config["bw"])
        sample_rate = int(config["sample_rate"])
        oversample = max(1, sample_rate // max(1, bw))
        samples_per_symbol = (1 << sf) * oversample
    except Exception:
        samples_per_symbol = 0
    tail_guard_symbols = int(config.get("tail_guard_symbols", 2) or 0)
    if samples_per_symbol > 0 and tail_guard_symbols > 0:
        capture_samples += tail_guard_symbols * samples_per_symbol + 64

    # Optional TX delay to prepend noise-only samples before the packet.
        tx_delay_symbols = int(config.get("tx_delay_symbols", 0) or 0)
        tx_delay_samples = int(config.get("tx_delay_samples", 0) or 0)
        if tx_delay_samples <= 0 and samples_per_symbol > 0 and tx_delay_symbols > 0:
            tx_delay_samples = tx_delay_symbols * samples_per_symbol
        capture_samples += tx_delay_samples

    # tx_rx_simulation.py uses a throttle set to samp_rate*10. Ensure autostop
    # is long enough for the flowgraph to actually emit capture_samples, else
    # high-SF packets get truncated and become undecodable.
    throttle_factor = float(config.get("throttle_factor", 10.0))
    sr = float(config.get("sample_rate", 0.0) or 0.0)
    computed_autostop = 0.25
    if capture_samples > 0 and sr > 0 and throttle_factor > 0:
        wall_needed = float(capture_samples) / (sr * throttle_factor)
        computed_autostop = max(0.25, wall_needed * 2.0 + 0.25)
    autostop_secs = float(config.get("autostop_secs", computed_autostop))

    env = os.environ.copy()
    env.update(
        {
            "PYTHONPATH": str(REPO_ROOT / "gr_lora_sdr" / "install" / "lib" / "python3.12" / "site-packages")
            + ":" + str(REPO_ROOT / "gr_lora_sdr" / "python"),
            "LD_LIBRARY_PATH": str(REPO_ROOT / "gr_lora_sdr" / "install" / "lib"),
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
                # Generate a single packet (avoid multi-frame ambiguity). We'll post-pad
                # the capture to a desired length for decoder robustness.
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

    print(f"[generate_sim_captures] Running {config['name']} -> {config['output_name']}")
    subprocess.run(cmd, env=env, check=True, cwd=str(REPO_ROOT), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    capture_path = output_dir / config["output_name"]
    if not capture_path.exists():
        raise FileNotFoundError(f"Expected capture not found: {capture_path}")

    size_bytes = capture_path.stat().st_size
    sample_count = size_bytes // 8  # complex float32

    pad_head_samples = int(config.get("pad_head_samples", 0) or 0)
    if pad_head_samples > 0:
        tmp_path = capture_path.with_suffix(capture_path.suffix + ".tmp")
        with tmp_path.open("wb") as out_f:
            out_f.write(b"\x00" * (pad_head_samples * 8))
            with capture_path.open("rb") as in_f:
                out_f.write(in_f.read())
        os.replace(tmp_path, capture_path)
        size_bytes = capture_path.stat().st_size
        sample_count = size_bytes // 8

    # Ensure deterministic capture length and provide tail padding without emitting
    # additional packets.
    if capture_samples > 0:
        if sample_count != capture_samples:
            target_bytes = int(capture_samples) * 8
            if size_bytes > target_bytes:
                with capture_path.open("r+b") as f:
                    f.truncate(target_bytes)
            else:
                pad_bytes = target_bytes - size_bytes
                if pad_bytes > 0:
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
        "capture_file": config["output_name"],
        "sample_count": sample_count,
        "duration_secs": sample_count / config["sample_rate"],
        "snr_db": config.get("snr_db", 0.0),
    }
    metadata_path = capture_path.with_suffix(".json")
    metadata_path.write_text(json.dumps(metadata, indent=2))
    print(f"[generate_sim_captures] Wrote metadata {metadata_path}")


CONFIGS = [
    {
        "name": "sf7_bw62500_cr1_short",
        "sf": 7,
        "bw": 62500,
        "sample_rate": 250000,
        "cr": 1,
        "payload_len": 24,
        "ldro": 0,
        "snr_db": 5.0,
        "output_name": "tx_rx_sf7_bw62500_cr1_snrp5p0_short.cf32",
    },
    {
        "name": "sf7_bw62500_cr3_implicithdr_nocrc",
        "sf": 7,
        "bw": 62500,
        "sample_rate": 250000,
        "cr": 3,
        "payload_len": 48,
        "ldro": 0,
        "implicit_header": True,
        "has_crc": False,
        "snr_db": -3.0,
        "output_name": "tx_rx_sf7_bw62500_cr3_implicithdr_nocrc.cf32",
    },
    {
        "name": "sf8_bw500000_cr3_long",
        "sf": 8,
        "bw": 500000,
        "sample_rate": 1000000,
        "cr": 3,
        "payload_len": 64,
        "ldro": 1,
        "snr_db": -2.0,
        "output_name": "tx_rx_sf8_bw500000_cr3_snrm2p0_long.cf32",
    },
    {
        "name": "sf8_bw125000_cr4_payload128",
        "sf": 8,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 4,
        "payload_len": 128,
        "ldro": 0,
        "snr_db": -8.0,
        "output_name": "tx_rx_sf8_bw125000_cr4_payload128.cf32",
    },
    {
        "name": "sf9_bw125000_cr1_no_crc",
        "sf": 9,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 1,
        "payload_len": 48,
        "ldro": 0,
        "snr_db": 0.0,
        "implicit_header": True,
        "has_crc": False,
        "output_name": "tx_rx_sf9_bw125000_cr1_implicithdr_nocrc.cf32",
    },
    {
        "name": "sf10_bw500000_cr4_highsnr",
        "sf": 10,
        "bw": 500000,
        "sample_rate": 1000000,
        "cr": 4,
        "payload_len": 32,
        "ldro": 1,
        "snr_db": 15.0,
        "output_name": "tx_rx_sf10_bw500000_cr4_snrp15p0_short.cf32",
    },
    {
        "name": "sf9_bw500000_cr2_snrneg10",
        "sf": 9,
        "bw": 500000,
        "sample_rate": 1000000,
        "cr": 2,
        "payload_len": 48,
        "ldro": 1,
        "snr_db": -10.0,
        "output_name": "tx_rx_sf9_bw500000_cr2_snrm10p0.cf32",
    },
    {
        "name": "sf6_bw62500_cr4_ldro",
        "sf": 6,
        "bw": 62500,
        "sample_rate": 250000,
        "cr": 4,
        "payload_len": 24,
        "ldro": 1,
        "snr_db": 2.0,
        "output_name": "tx_rx_sf6_bw62500_cr4_ldro.cf32",
    },
    {
        "name": "sf12_bw500_cr4_payload256",
        "sf": 12,
        "bw": 500000,
        "sample_rate": 1000000,
        "cr": 4,
        "payload_len": 256,
        "ldro": 1,
        "implicit_header": True,
        "has_crc": False,
        "snr_db": -15.0,
        "output_name": "tx_rx_sf12_bw500000_cr4_payload256.cf32",
    },
    {
        "name": "sf10_bw62500_cr2_ldro",
        "sf": 10,
        "bw": 62500,
        "sample_rate": 250000,
        "cr": 2,
        "payload_len": 48,
        "ldro": 1,
        "snr_db": 3.0,
        "output_name": "tx_rx_sf10_bw62500_cr2_ldro.cf32",
    },
    {
        "name": "sf7_bw250000_cr1_clean",
        "sf": 7,
        "bw": 250000,
        "sample_rate": 1000000,
        "cr": 1,
        "payload_len": 16,
        "ldro": 0,
        "snr_db": 0.0,
        "output_name": "tx_rx_sf7_bw250000_cr1.cf32",
    },
    {
        "name": "sf7_bw125000_cr1_short",
        "sf": 7,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 1,
        "payload_len": 16,
        "ldro": 0,
        "snr_db": -5.0,
        "output_name": "tx_rx_sf7_bw125000_cr1_snrm5p0_short.cf32",
    },
    {
        "name": "sf7_bw125000_cr1_short_highsnr",
        "sf": 7,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 1,
        "payload_len": 16,
        "ldro": 0,
        "snr_db": 15.0,
        "capture_margin": 3.0,
        "pad_head_samples": 0,
        "output_name": "tx_rx_sf7_bw125000_cr1_snrp15p0_short.cf32",
    },
    {
        "name": "sf9_bw125000_cr3_short",
        "sf": 9,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 3,
        "payload_len": 16,
        "ldro": 0,
        "snr_db": -2.0,
        "output_name": "tx_rx_sf9_bw125000_cr3_snrm2p0_short.cf32",
    },
    {
        "name": "sf9_bw125000_cr3_short_highsnr",
        "sf": 9,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 3,
        "payload_len": 16,
        "ldro": 0,
        "snr_db": 15.0,
        "capture_margin": 1.6,
        "output_name": "tx_rx_sf9_bw125000_cr3_snrp15p0_short.cf32",
    },
    {
        "name": "sf5_bw125000_cr1_full",
        "sf": 5,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 1,
        "payload_len": 255,
        "ldro": 0,
        "snr_db": -5.0,
        "output_name": "tx_rx_sf5_bw125000_cr1_snrm5p0.cf32",
    },
    {
        "name": "sf5_bw125000_cr2_full",
        "sf": 5,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 2,
        "payload_len": 255,
        "ldro": 0,
        "snr_db": -5.0,
        "output_name": "tx_rx_sf5_bw125000_cr2_snrm5p0.cf32",
    },
    {
        "name": "sf6_bw125000_cr1_full",
        "sf": 6,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 1,
        "payload_len": 255,
        "ldro": 0,
        "snr_db": -5.0,
        "output_name": "tx_rx_sf6_bw125000_cr1_snrm5p0.cf32",
    },
    {
        "name": "sf6_bw125000_cr2_full",
        "sf": 6,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 2,
        "payload_len": 255,
        "ldro": 0,
        "snr_db": -5.0,
        "output_name": "tx_rx_sf6_bw125000_cr2_snrm5p0.cf32",
    },
    {
        "name": "sf10_bw250000_cr4_full",
        "sf": 10,
        "bw": 250000,
        "sample_rate": 1000000,
        "cr": 4,
        "payload_len": 255,
        "ldro": 0,
        "snr_db": 0.0,
        "output_name": "tx_rx_sf10_bw250000_cr4_snrp0p0.cf32",
    },
    {
        "name": "sf7_bw125000_cr2_short",
        "sf": 7,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 2,
        "payload_len": 16,
        "ldro": 0,
        "snr_db": -5.0,
        "output_name": "tx_rx_sf7_bw125000_cr2_snrm5p0_short.cf32",
    },
    {
        "name": "sf7_bw125000_cr2_full",
        "sf": 7,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 2,
        "payload_len": 255,
        "ldro": 0,
        "snr_db": -5.0,
        "output_name": "tx_rx_sf7_bw125000_cr2_snrm5p0.cf32",
    },
    {
        "name": "sf11_bw125000_cr1_short",
        "sf": 11,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 1,
        "payload_len": 24,
        "ldro": 1,
        "snr_db": 0.0,
        "tx_delay_symbols": 1,
        "pad_head_samples": 0,
        "output_name": "tx_rx_sf11_bw125000_cr1_snrm0p0_short.cf32",
    },
    {
        "name": "sf12_bw125000_cr1_short",
        "sf": 12,
        "bw": 125000,
        "sample_rate": 500000,
        "cr": 1,
        "payload_len": 24,
        "ldro": 1,
        "snr_db": 0.0,
        "tx_delay_symbols": 1,
        "pad_head_samples": 0,
        "output_name": "tx_rx_sf12_bw125000_cr1_snrm0p0_short.cf32",
    },
    {
        "name": "sf7_bw500000_cr2_implicithdr_nocrc",
        "sf": 7,
        "bw": 500000,
        "sample_rate": 1000000,
        "cr": 2,
        "payload_len": 32,
        "ldro": 0,
        "implicit_header": True,
        "has_crc": False,
        "snr_db": 0.0,
        "output_name": "tx_rx_sf7_bw500000_cr2_implicithdr_nocrc.cf32",
    },
]

# Add systematic sweep (including SF5, SF6, SF12)
for sf in [5, 6, 7, 8, 9, 10, 11, 12]:
    for bw in [125000, 250000, 500000]:
        for cr in [1, 2, 3, 4]:
            # Skip invalid combinations if any (e.g. SF6 with implicit header is mandatory? No, explicit is fine)
            
            # Calculate symbol duration for LDRO
            symbol_duration = (2**sf) / float(bw)
            ldro = 1 if symbol_duration > 0.016 else 0
            
            # Sample rate logic
            if bw == 500000:
                sample_rate = 1000000
            elif bw == 250000:
                sample_rate = 500000 # or 1M
            else:
                sample_rate = 250000 # or 500k
                
            # Ensure sample rate is sufficient
            if sample_rate < bw:
                sample_rate = bw * 2

            name = f"sweep_sf{sf}_bw{int(bw/1000)}k_cr{cr}"
            
            # Check if already exists in manual configs to avoid duplicates
            if any(c["name"] == name for c in CONFIGS):
                continue
                
            CONFIGS.append({
                "name": name,
                "sf": sf,
                "bw": bw,
                "sample_rate": sample_rate,
                "cr": cr,
                "payload_len": 32,
                "ldro": ldro,
                "snr_db": 20.0, # High SNR for logic verification
                "output_name": f"{name}.cf32"
            })

# === Extended test coverage ===

# 1. Implicit Header sweep (SF7-SF12, BW125k, CR1-4)
for sf in [7, 8, 9, 10, 11, 12]:
    for cr in [1, 2, 3, 4]:
        bw = 125000
        sample_rate = 250000
        symbol_duration = (2**sf) / float(bw)
        ldro = 1 if symbol_duration > 0.016 else 0
        name = f"implicit_sf{sf}_bw125k_cr{cr}"
        if not any(c["name"] == name for c in CONFIGS):
            CONFIGS.append({
                "name": name,
                "sf": sf,
                "bw": bw,
                "sample_rate": sample_rate,
                "cr": cr,
                "payload_len": 32,
                "ldro": ldro,
                "implicit_header": True,
                "has_crc": True,
                "snr_db": 20.0,
                "output_name": f"{name}.cf32"
            })

# 2. Preamble length variations (SF8, BW125k, CR1)
for preamble_len in [6, 10, 12, 16, 20]:
    name = f"preamble{preamble_len}_sf8_bw125k"
    if not any(c["name"] == name for c in CONFIGS):
        CONFIGS.append({
            "name": name,
            "sf": 8,
            "bw": 125000,
            "sample_rate": 250000,
            "cr": 1,
            "payload_len": 32,
            "preamble_len": preamble_len,
            "ldro": 0,
            "snr_db": 20.0,
            "output_name": f"{name}.cf32"
        })

# 3. Edge payload lengths (SF8, BW125k, CR1)
for payload_len in [1, 2, 4, 8, 251, 252, 253, 254]:
    name = f"payload{payload_len}_sf8_bw125k"
    if not any(c["name"] == name for c in CONFIGS):
        CONFIGS.append({
            "name": name,
            "sf": 8,
            "bw": 125000,
            "sample_rate": 250000,
            "cr": 1,
            "payload_len": payload_len,
            "ldro": 0,
            "snr_db": 20.0,
            "output_name": f"{name}.cf32"
        })

# 4. SF5/SF6 with implicit header
for sf in [5, 6]:
    for cr in [1, 4]:
        bw = 125000
        sample_rate = 250000
        name = f"implicit_sf{sf}_bw125k_cr{cr}"
        if not any(c["name"] == name for c in CONFIGS):
            CONFIGS.append({
                "name": name,
                "sf": sf,
                "bw": bw,
                "sample_rate": sample_rate,
                "cr": cr,
                "payload_len": 32,
                "ldro": 0,
                "implicit_header": True,
                "has_crc": True,
                "snr_db": 20.0,
                "output_name": f"{name}.cf32"
            })

# 5. Low BW (62.5kHz) extended coverage
for sf in [7, 8, 9, 10]:
    for cr in [1, 4]:
        bw = 62500
        sample_rate = 250000
        symbol_duration = (2**sf) / float(bw)
        ldro = 1 if symbol_duration > 0.016 else 0
        name = f"lowbw_sf{sf}_bw62k_cr{cr}"
        if not any(c["name"] == name for c in CONFIGS):
            CONFIGS.append({
                "name": name,
                "sf": sf,
                "bw": bw,
                "sample_rate": sample_rate,
                "cr": cr,
                "payload_len": 32,
                "ldro": ldro,
                "snr_db": 20.0,
                "output_name": f"{name}.cf32"
            })

# 6. No-CRC extended (explicit header, no CRC)
for sf in [7, 8, 9]:
    name = f"nocrc_sf{sf}_bw125k_cr1"
    if not any(c["name"] == name for c in CONFIGS):
        CONFIGS.append({
            "name": name,
            "sf": sf,
            "bw": 125000,
            "sample_rate": 250000,
            "cr": 1,
            "payload_len": 32,
            "ldro": 0,
            "has_crc": False,
            "snr_db": 20.0,
            "output_name": f"{name}.cf32"
        })



def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--only",
        nargs="*",
        default=None,
        help="Generate only named configs (matches the 'name' field).",
    )
    parser.add_argument(
        "--output-dir",
        default=str(OUTPUT_DIR),
        help="Output directory for generated CF32 + JSON metadata.",
    )
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    selected = CONFIGS
    if args.only is not None and len(args.only) > 0:
        wanted = set(args.only)
        selected = [cfg for cfg in CONFIGS if cfg.get("name") in wanted]
        missing = sorted(wanted - {cfg.get("name") for cfg in selected})
        if missing:
            raise SystemExit(f"Unknown config name(s): {', '.join(missing)}")

    for cfg in selected:
        run_capture(cfg, output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
