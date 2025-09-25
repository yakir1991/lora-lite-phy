#!/usr/bin/env python3
"""Decode an offline LoRa baseband capture using the receiver chain.

The script reads complex32 IQ samples from disk, feeds them to the 
 :class:`gnuradio.lora_sdr.lora_sdr_lora_rx` hierarchical block and prints
 the recovered payloads together with CRC information.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import numpy as np
import pmt
from gnuradio import blocks, gr
import gnuradio.lora_sdr as lora_sdr


@dataclass
class FrameResult:
    index: int
    payload: bytes
    crc_valid: Optional[bool]
    has_crc: Optional[bool]
    message_text: Optional[str]

    def hex_payload(self) -> str:
        return " ".join(f"{byte:02x}" for byte in self.payload)


class MessageCollector(gr.sync_block):
    """Collect messages emitted on a GNU Radio message port."""

    def __init__(self):
        super().__init__(name="message_collector", in_sig=None, out_sig=None)
        self.message_port_register_in(pmt.intern("in"))
        self.set_msg_handler(pmt.intern("in"), self._handle_msg)
        self.messages: List[bytes] = []

    def _handle_msg(self, msg: pmt.pmt_t) -> None:
        if pmt.is_symbol(msg):
            text = pmt.symbol_to_string(msg)
            self.messages.append(text.encode("latin-1", errors="ignore"))
        elif pmt.is_u8vector(msg):
            self.messages.append(bytes(pmt.u8vector_elements(msg)))
        else:
            self.messages.append(pmt.serialize_str(msg))

    def work(self, input_items, output_items):  # pragma: no cover - message only block
        return 0


class OfflineLoraReceiver(gr.top_block):
    def __init__(
        self,
        iq_samples: Sequence[complex],
        samp_rate: float,
        bw: float,
        sf: int,
        cr: int,
        has_crc: bool,
        impl_head: bool,
        ldro_mode: int,
        sync_word: Sequence[int],
        pay_len: int,
        soft_decoding: bool = False,
        center_freq: float = 868.1e6,
        print_header: bool = True,
        print_payload: bool = True,
    ) -> None:
        super().__init__("offline_lora_receiver", catch_exceptions=True)

        center_freq = int(round(center_freq))
        bw = int(round(bw))
        samp_rate = int(round(samp_rate))

        self.src = blocks.vector_source_c(list(iq_samples), False)
        self.rx = lora_sdr.lora_sdr_lora_rx(
            center_freq=center_freq,
            bw=bw,
            cr=cr,
            has_crc=has_crc,
            impl_head=impl_head,
            pay_len=pay_len,
            samp_rate=samp_rate,
            sf=sf,
            sync_word=list(sync_word),
            soft_decoding=soft_decoding,
            ldro_mode=ldro_mode,
            print_rx=[print_header, print_payload],
        )
        self.payload_sink = blocks.vector_sink_b()
        self.msg_collector = MessageCollector()

        self.connect(self.src, self.rx)
        self.connect(self.rx, self.payload_sink)
        self.msg_connect((self.rx, "out"), (self.msg_collector, "in"))

    def results(self) -> Tuple[bytes, Iterable[gr.tag_t], List[bytes]]:
        payload_bytes = bytes(self.payload_sink.data())
        return payload_bytes, list(self.payload_sink.tags()), list(self.msg_collector.messages)


def parse_numeric_suffix(value: str) -> float:
    match = re.fullmatch(r"(?P<base>\d+(?:\.\d+)?)(?P<suffix>[kKmMgG]?)", value)
    if not match:
        raise ValueError(f"Cannot parse numeric value '{value}'")
    base = float(match.group("base"))
    suffix = match.group("suffix").lower()
    scale = {"": 1.0, "k": 1e3, "m": 1e6, "g": 1e9}[suffix]
    return base * scale


def infer_parameters_from_name(stem: str) -> Dict[str, object]:
    pattern = re.compile(
        r"sps_(?P<sps>[^_]+)_bw_(?P<bw>[^_]+)_sf_(?P<sf>\d+)_cr_(?P<cr>\d+)"
        r"_ldro_(?P<ldro>[^_]+)_crc_(?P<crc>[^_]+)_implheader_(?P<impl>[^_]+)"
    )
    match = pattern.search(stem)
    if not match:
        return {}

    params: Dict[str, object] = {
        "samp_rate": parse_numeric_suffix(match.group("sps")),
        "bw": parse_numeric_suffix(match.group("bw")),
        "sf": int(match.group("sf")),
        "cr": int(match.group("cr")),
        "ldro_mode": 1 if match.group("ldro").lower() == "true" else 0,
        "has_crc": match.group("crc").lower() == "true",
        "impl_head": match.group("impl").lower() == "true",
    }
    return params


def extract_frames(payload_bytes: bytes, tags: Iterable[gr.tag_t]) -> List[FrameResult]:
    frames: List[FrameResult] = []
    cursor = 0
    sorted_tags = sorted(tags, key=lambda t: t.offset)

    for tag in sorted_tags:
        key = pmt.symbol_to_string(tag.key)
        if key != "frame_info":
            continue
        info = tag.value
        pay_len_pmt = pmt.dict_ref(info, pmt.string_to_symbol("pay_len"), pmt.PMT_NIL)
        if pmt.is_integer(pay_len_pmt):
            pay_len = int(pmt.to_long(pay_len_pmt))
        else:
            pay_len = 0

        crc_present_pmt = pmt.dict_ref(info, pmt.string_to_symbol("crc"), pmt.PMT_NIL)
        if pmt.is_null(crc_present_pmt):
            crc_present = None
        elif pmt.is_bool(crc_present_pmt):
            crc_present = pmt.to_bool(crc_present_pmt)
        elif pmt.is_integer(crc_present_pmt):
            crc_present = bool(pmt.to_long(crc_present_pmt))
        else:
            crc_present = None

        crc_valid_pmt = pmt.dict_ref(info, pmt.string_to_symbol("crc_valid"), pmt.PMT_NIL)
        crc_valid = pmt.to_bool(crc_valid_pmt) if not pmt.is_null(crc_valid_pmt) else None

        payload = payload_bytes[cursor:cursor + pay_len]
        cursor += pay_len

        frames.append(
            FrameResult(
                index=len(frames),
                payload=payload,
                crc_valid=crc_valid,
                has_crc=crc_present,
                message_text=None,
            )
        )

    return frames


def combine_messages(frames: List[FrameResult], messages: Sequence[bytes]) -> None:
    for frame, msg_bytes in zip(frames, messages):
        try:
            frame.message_text = msg_bytes.decode("utf-8")
        except UnicodeDecodeError:
            frame.message_text = msg_bytes.decode("latin-1", errors="replace")


def _load_iq(file_path: Path, fmt: str) -> np.ndarray:
    if fmt == "cf32":
        iq = np.fromfile(file_path, dtype=np.complex64)
    elif fmt == "f32":
        raw = np.fromfile(file_path, dtype=np.float32)
        if raw.size % 2 != 0:
            raise ValueError(f"Expected an even number of float32 entries in {file_path}")
        iq = raw.astype(np.float32).view(np.complex64)
    elif fmt == "cs16":
        raw = np.fromfile(file_path, dtype=np.int16)
        if raw.size % 2 != 0:
            raise ValueError(f"Expected an even number of int16 entries in {file_path}")
        reshaped = raw.astype(np.float32).reshape(-1, 2)
        iq = (reshaped[:, 0] + 1j * reshaped[:, 1]) / 32768.0
    else:
        raise ValueError(f"Unsupported IQ format '{fmt}'")
    return iq.astype(np.complex64)


def main() -> None:
    parser = argparse.ArgumentParser(description="Decode a stored LoRa IQ vector")
    parser.add_argument("input", type=Path, help="Path to the binary IQ capture")
    parser.add_argument("--sf", type=int, help="Spreading factor")
    parser.add_argument("--bw", type=float, help="Signal bandwidth in Hz")
    parser.add_argument("--samp-rate", type=float, help="Sampling rate in Hz")
    parser.add_argument("--cr", type=int, help="Coding rate (1-4)")
    parser.add_argument("--has-crc", dest="has_crc", action="store_true", help="Force CRC usage")
    parser.add_argument("--no-crc", dest="has_crc", action="store_false", help="Force CRC disabled")
    parser.add_argument("--impl-header", dest="impl_head", action="store_true", help="Use implicit header")
    parser.add_argument("--explicit-header", dest="impl_head", action="store_false", help="Use explicit header")
    parser.add_argument("--ldro-mode", type=int, choices=[0, 1, 2], help="Low data-rate optimisation mode (0=off,1=on,2=auto)")
    parser.add_argument("--sync-word", type=lambda x: int(x, 0), default=0x12, help="Sync word as integer")
    parser.add_argument("--pay-len", type=int, default=255, help="Maximum payload length")
    parser.add_argument("--center-freq", type=float, default=868.1e6, help="Center frequency in Hz")
    parser.add_argument("--format", choices=["cf32", "f32", "cs16"], default="cf32", help="IQ file format")
    parser.add_argument("--limit", type=int, default=None, help="Process only the first N IQ samples")
    parser.set_defaults(has_crc=None, impl_head=None)

    args = parser.parse_args()

    file_path = args.input.expanduser().resolve()
    if not file_path.exists():
        raise FileNotFoundError(file_path)

    inferred = infer_parameters_from_name(file_path.stem)

    sf = args.sf or inferred.get("sf")
    bw = args.bw or inferred.get("bw")
    samp_rate = args.samp_rate or inferred.get("samp_rate", bw)
    cr = args.cr or inferred.get("cr", 1)
    has_crc = args.has_crc if args.has_crc is not None else inferred.get("has_crc", True)
    impl_head = args.impl_head if args.impl_head is not None else inferred.get("impl_head", False)
    ldro_mode = args.ldro_mode if args.ldro_mode is not None else inferred.get("ldro_mode", 2)

    if sf is None or bw is None or samp_rate is None:
        raise ValueError("Spreading factor, bandwidth and sample rate must be provided")

    iq = _load_iq(file_path, args.format)
    if args.limit is not None:
        iq = iq[:args.limit]

    receiver = OfflineLoraReceiver(
        iq_samples=iq,
        samp_rate=samp_rate,
        bw=bw,
        sf=sf,
        cr=cr,
        has_crc=has_crc,
        impl_head=impl_head,
        ldro_mode=ldro_mode,
        sync_word=[args.sync_word],
        pay_len=args.pay_len,
        center_freq=args.center_freq,
    )

    receiver.run()
    payload_bytes, tags, messages = receiver.results()
    frames = extract_frames(payload_bytes, tags)
    combine_messages(frames, messages)

    if not frames:
        print("No frames were decoded")
        return

    for frame in frames:
        crc_state = "unknown"
        if frame.crc_valid is True:
            crc_state = "valid"
        elif frame.crc_valid is False:
            crc_state = "invalid"
        elif frame.has_crc is False:
            crc_state = "not present"
        print(
            f"Frame {frame.index}: {len(frame.payload)} bytes, CRC {crc_state}\n"
            f"  Hex: {frame.hex_payload()}"
        )
        if frame.message_text is not None:
            print(f"  Text: {frame.message_text}")


if __name__ == "__main__":
    main()
