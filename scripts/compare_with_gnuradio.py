#!/usr/bin/env python3
"""Compare standalone lora_rx output against a GNU Radio + gr-lora_sdr reference decode.

This script expects the standalone decoder to be built at `standalone/build/lora_rx`.
For the GNU Radio comparison we rely on the `gnuradio` Python bindings and the
`gnuradio.lora_sdr` blocks that ship with the upstream reference project.  If the
modules are not importable (e.g., the conda environment is not active) we emit a
clear error so the user can activate the provided environment (`conda activate gr310`,
see `external/gr_lora_sdr/environment.yml`).
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple

import numpy as np

CLI_PATH = Path("standalone/build/lora_rx")


def _load_iq(path: Path, fmt: str) -> np.ndarray:
    if fmt == "f32":
        data = np.fromfile(path, dtype=np.float32)
        if data.size % 2 != 0:
            raise ValueError(f"Expected an even number of float32 entries in {path}")
        iq = data.view(np.complex64)
    elif fmt == "cs16":
        data = np.fromfile(path, dtype=np.int16)
        if data.size % 2 != 0:
            raise ValueError(f"Expected an even number of int16 entries in {path}")
        reshaped = data.astype(np.float32).reshape(-1, 2)
        iq = (reshaped[:, 0] + 1j * reshaped[:, 1]) / 32768.0
    else:
        raise ValueError(f"Unsupported format: {fmt}")
    return iq.astype(np.complex64)


def _run_standalone(vector: Path, fmt: str, sf: int, bw: int, fs: int,
                    extra_args: Optional[Sequence[str]] = None) -> dict:
    args = [str(CLI_PATH), "--json"]
    if fmt:
        args.extend(["--format", fmt])
    if extra_args:
        args.extend(extra_args)
    args.extend([str(vector), str(sf), str(bw), str(fs)])
    res = subprocess.run(args, capture_output=True, text=True, check=True)
    try:
        return json.loads(res.stdout.strip())
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Failed to parse lora_rx JSON output:\n{res.stdout}\n{res.stderr}") from exc


@dataclass
class GRDecodeResult:
    payload: List[int]
    has_crc: bool
    header_len: int
    header_cr: int
    header_crc_flag: bool
    frame_info: dict


def _run_gnuradio(samples: np.ndarray, sf: int, bw: int, fs: int,
                  preamble_len: int, sync_words: Sequence[int],
                  impl_header: bool, center_freq: float) -> GRDecodeResult:
    try:
        from gnuradio import blocks, gr
        import gnuradio.lora_sdr as lora_sdr
        import pmt
    except ModuleNotFoundError as exc:
        raise RuntimeError(
            "gnuradio or gnuradio.lora_sdr not importable. Activate the provided conda "
            "environment first (e.g. `conda activate gr310`)."
        ) from exc

    class FrameInfoCollector(gr.basic_block):
        def __init__(self):
            gr.basic_block.__init__(self, "frame_info_collector", [], [])
            self.message_port_register_in(pmt.intern("frame_info"))
            self.set_msg_handler(pmt.intern("frame_info"), self._handle)
            self.frames: List[dict] = []

        def _handle(self, msg):
            self.frames.append(pmt.to_python(msg))

    class CRCCollector(gr.basic_block):
        def __init__(self):
            gr.basic_block.__init__(self, "crc_collector", [], [])
            self.message_port_register_in(pmt.intern("msg"))
            self.set_msg_handler(pmt.intern("msg"), self._handle)
            self.messages: List[dict] = []

        def _handle(self, msg):
            self.messages.append(pmt.to_python(msg))

    class Top(gr.top_block):
        def __init__(self, iq: np.ndarray):
            gr.top_block.__init__(self, "gr_lora_sdr_offline")
            self.src = blocks.vector_source_c(iq.tolist(), False, 1, [])
            soft = False
            ldro_mode = 2  # auto
            self.frame_sync = lora_sdr.frame_sync(int(center_freq), bw, sf, impl_header, list(sync_words), int(fs / bw), preamble_len)
            self.fft_demod = lora_sdr.fft_demod(soft, True)
            self.gray_mapping = lora_sdr.gray_mapping(soft)
            self.deinterleaver = lora_sdr.deinterleaver(soft)
            self.hamming_dec = lora_sdr.hamming_dec(soft)
            self.header_decoder = lora_sdr.header_decoder(impl_header, 1, 255, True, ldro_mode, False)
            self.dewhitening = lora_sdr.dewhitening()
            self.crc_verif = lora_sdr.crc_verif(True, False)
            self.sink = blocks.vector_sink_b()
            self.frame_info = FrameInfoCollector()
            self.crc_info = CRCCollector()

            # stream connections
            self.connect(self.src, self.frame_sync)
            self.connect(self.frame_sync, self.fft_demod)
            self.connect(self.fft_demod, self.gray_mapping)
            self.connect(self.gray_mapping, self.deinterleaver)
            self.connect(self.deinterleaver, self.hamming_dec)
            self.connect(self.hamming_dec, self.header_decoder)
            self.connect(self.header_decoder, self.dewhitening)
            self.connect(self.dewhitening, self.crc_verif)
            self.connect(self.crc_verif, self.sink)

            # message wiring
            self.msg_connect(self.header_decoder, 'frame_info', self.frame_sync, 'frame_info')
            self.msg_connect(self.header_decoder, 'frame_info', self.frame_info, 'frame_info')
            self.msg_connect(self.crc_verif, 'msg', self.crc_info, 'msg')

    tb = Top(samples)
    tb.run()

    payload = list(tb.sink.data())
    frame_info = tb.frame_info.frames[-1] if tb.frame_info.frames else {}
    crc_msgs = tb.crc_info.messages
    has_crc_flag = bool(frame_info.get('crc', 0)) if frame_info else False
    header_len = int(frame_info.get('pay_len', 0)) if frame_info else 0
    header_cr = int(frame_info.get('cr', 0)) if frame_info else 0

    return GRDecodeResult(
        payload=payload,
        has_crc=has_crc_flag,
        header_len=header_len,
        header_cr=header_cr,
        header_crc_flag=has_crc_flag,
        frame_info=frame_info,
    )


def _format_bytes(data: Iterable[int]) -> str:
    return ' '.join(f"{b:02X}" for b in data)


def compare(vector: Path, fmt: str, sf: int, bw: int, fs: int,
            preamble_len: int, sync_words: Sequence[int], impl_header: bool,
            center_freq: float,
            extra_lora_args: Optional[Sequence[str]] = None) -> None:
    standalone = _run_standalone(vector, fmt, sf, bw, fs, extra_lora_args)
    samples = _load_iq(vector, fmt)
    gr_result = _run_gnuradio(samples, sf, bw, fs, preamble_len, sync_words, impl_header, center_freq)

    print("Standalone header:", {
        "payload_len": standalone.get("payload_len"),
        "cr_idx": standalone.get("cr_idx"),
        "has_crc": standalone.get("has_crc"),
        "header_crc_ok": standalone.get("header_crc_ok"),
    })
    print("GNU Radio header:", {
        "payload_len": gr_result.header_len,
        "cr": gr_result.header_cr,
        "has_crc": gr_result.has_crc,
    })

    stand_payload = standalone.get("payload_bytes", [])
    gr_payload = gr_result.payload

    limit = max(len(stand_payload), len(gr_payload))
    diffs = []
    for idx in range(limit):
        st = stand_payload[idx] if idx < len(stand_payload) else None
        gr_val = gr_payload[idx] if idx < len(gr_payload) else None
        if st != gr_val:
            diffs.append((idx, st, gr_val))

    print(f"Standalone payload ({len(stand_payload)} bytes): {_format_bytes(stand_payload[:32])}{' ...' if len(stand_payload) > 32 else ''}")
    print(f"GNU Radio payload ({len(gr_payload)} bytes): {_format_bytes(gr_payload[:32])}{' ...' if len(gr_payload) > 32 else ''}")

    if diffs:
        print("\nPayload differences (index, standalone, gnuradio):")
        for idx, st, gr_val in diffs[:40]:
            print(f"  {idx:03d}: {st!r} vs {gr_val!r}")
        if len(diffs) > 40:
            print(f"  ... ({len(diffs) - 40} more)" )
    else:
        print("\nPayload bytes match.")


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare standalone decoder output with GNU Radio")
    parser.add_argument("vector", type=Path, help="Path to IQ capture")
    parser.add_argument("sf", type=int, help="Spreading factor")
    parser.add_argument("bw", type=int, help="Bandwidth (Hz)")
    parser.add_argument("fs", type=int, help="Sample rate (Hz)")
    parser.add_argument("--format", default="f32", choices=["f32", "cs16"], help="Input IQ format")
    parser.add_argument("--preamble", type=int, default=8, help="Preamble symbols")
    parser.add_argument("--sync", type=lambda s: int(s, 0), default=[0x34], nargs='+', help="Sync word(s) to assume")
    parser.add_argument("--impl-header", action="store_true", help="Vector uses implicit header")
    parser.add_argument("--center-freq", type=float, default=868.1e6, help="Center frequency in Hz")
    parser.add_argument("--extra-standalone-args", nargs=argparse.REMAINDER,
                        help="Additional arguments to forward to standalone lora_rx")
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> None:
    args = parse_args(argv)
    if not CLI_PATH.exists():
        raise SystemExit(f"Standalone decoder not found at {CLI_PATH}. Build it first.")
    compare(
        vector=args.vector,
        fmt=args.format,
        sf=args.sf,
        bw=args.bw,
        fs=args.fs,
        preamble_len=args.preamble,
        sync_words=args.sync,
        impl_header=args.impl_header,
        center_freq=args.center_freq,
        extra_lora_args=args.extra_standalone_args,
    )


if __name__ == "__main__":
    main()
