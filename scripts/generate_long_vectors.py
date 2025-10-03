#!/usr/bin/env python3
import json
from pathlib import Path
import sys
import struct
import os

import numpy as np

# Ensure numpy has Inf for any legacy code
if not hasattr(np, "Inf"):
    np.Inf = np.inf  # type: ignore

ROOT = Path(__file__).resolve().parents[1]
SDR_LORA = ROOT / "external" / "sdr_lora"
if str(SDR_LORA) not in sys.path:
    sys.path.insert(0, str(SDR_LORA))

try:
    import lora as sdr_lora  # type: ignore
except Exception as e:
    print(f"Failed to import external sdr_lora: {e}")
    sys.exit(2)


def write_cf32(path: Path, samples: np.ndarray):
    path.parent.mkdir(parents=True, exist_ok=True)
    # write interleaved float32 IQ
    with open(path, 'wb') as f:
        for c in samples.astype(np.complex64):
            f.write(struct.pack('<f', float(np.real(c))))
            f.write(struct.pack('<f', float(np.imag(c))))


def make_payload_hex(length: int) -> str:
    # Generate a long ASCII payload repeating a pattern
    msg = ("LORA_LONG_PAYLOAD_DEMO_".encode('ascii') * ((length // 24) + 1))[:length]
    return msg.hex()


def main():
    out_dir = ROOT / "vectors" / "synthetic_long"
    out_dir.mkdir(parents=True, exist_ok=True)

    # Parameters
    sf = 7
    bw = 125000
    fs = 500000
    cr = 2  # coding rate 4/6
    has_crc = 1
    preamble_bits = 8
    f0 = 0  # baseband

    # Long payload length in bytes
    payload_len = 40
    payload_hex = make_payload_hex(payload_len)
    payload_bytes = bytes.fromhex(payload_hex)

    # Explicit header vector (impl0)
    samples_impl0 = sdr_lora.encode(
        f0=f0,
        SF=sf,
        BW=bw,
        payload=np.frombuffer(payload_bytes, dtype=np.uint8),
        fs=fs,
        src=1,
        dst=2,
        seqn=7,
        cr=cr,
        enable_crc=has_crc,
        implicit_header=0,
        preamble_bits=preamble_bits,
    )
    cf32_impl0 = out_dir / f"tx_sf{sf}_bw{bw}_cr{cr}_crc{has_crc}_impl0_ldro0_pay{payload_len}.cf32"
    write_cf32(cf32_impl0, samples_impl0)
    meta_impl0 = {
        "filename": cf32_impl0.name,
        "sf": sf,
        "bw": bw,
        "samp_rate": fs,
        "cr": cr,
        "crc": bool(has_crc),
        "impl_header": False,
        "ldro_mode": 0,
        "payload_len": payload_len,
        "payload_hex": payload_hex,
        "sync_word": 0x12,
    }
    (cf32_impl0.with_suffix('.json')).write_text(json.dumps(meta_impl0, indent=2))

    # Implicit header vector (impl1): requires decoder overrides
    samples_impl1 = sdr_lora.encode(
        f0=f0,
        SF=sf,
        BW=bw,
        payload=np.frombuffer(payload_bytes, dtype=np.uint8),
        fs=fs,
        src=1,
        dst=2,
        seqn=8,
        cr=cr,
        enable_crc=has_crc,
        implicit_header=1,
        preamble_bits=preamble_bits,
    )
    cf32_impl1 = out_dir / f"tx_sf{sf}_bw{bw}_cr{cr}_crc{has_crc}_impl1_ldro0_pay{payload_len}.cf32"
    write_cf32(cf32_impl1, samples_impl1)
    meta_impl1 = {
        "filename": cf32_impl1.name,
        "sf": sf,
        "bw": bw,
        "samp_rate": fs,
        "cr": cr,
        "crc": bool(has_crc),
        "impl_header": True,
        "ldro_mode": 0,
        "payload_len": payload_len,
        "payload_hex": payload_hex,
        "sync_word": 0x12,
    }
    (cf32_impl1.with_suffix('.json')).write_text(json.dumps(meta_impl1, indent=2))

    print("Generated:")
    print(f" - {cf32_impl0}")
    print(f" - {cf32_impl1}")


if __name__ == "__main__":
    main()
