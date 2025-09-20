#!/usr/bin/env python3
"""Decode an offline LoRa baseband capture using the receiver chain.

The script reads complex32 IQ samples from disk, feeds them to the 
 :class:`gnuradio.lora_sdr.lora_sdr_lora_rx` hierarchical block and prints
 the recovered payloads together with CRC information.
"""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

import numpy as np
import pmt
from gnuradio import blocks, gr
import gnuradio.lora_sdr as lora_sdr

FRAME_INFO_KEYS: Tuple[str, ...] = (
    "pay_len",
    "cr",
    "crc",
    "crc_valid",
    "ldro_mode",
    "ldro",
    "err",
    "symb_numb",
    "is_header",
    "cfo_int",
    "cfo_frac",
    "sf",
)


@dataclass
class FrameResult:
    index: int
    payload: bytes
    crc_valid: Optional[bool]
    has_crc: Optional[bool]
    message_text: Optional[str]
    tag_offset: Optional[int]
    tag_info: Dict[str, Any]

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

        self.src = blocks.vector_source_c(list(iq_samples), False)
        self.rx = lora_sdr.lora_sdr_lora_rx(
            center_freq=int(center_freq),
            bw=int(bw),
            cr=cr,
            has_crc=has_crc,
            impl_head=impl_head,
            pay_len=pay_len,
            samp_rate=int(samp_rate),
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


def pmt_to_python(obj: pmt.pmt_t) -> Any:
    if pmt.is_null(obj):
        return None
    if pmt.is_bool(obj):
        return pmt.to_bool(obj)
    if pmt.is_integer(obj):
        return int(pmt.to_long(obj))
    if pmt.is_real(obj):
        return float(pmt.to_double(obj))
    if pmt.is_complex(obj):
        cmplx = pmt.to_complex(obj)
        return [cmplx.real, cmplx.imag]
    if pmt.is_symbol(obj):
        return pmt.symbol_to_string(obj)
    if pmt.is_u8vector(obj):
        return list(pmt.u8vector_elements(obj))
    if pmt.is_pair(obj):
        return [pmt_to_python(pmt.car(obj)), pmt_to_python(pmt.cdr(obj))]
    if pmt.is_dict(obj):
        result: Dict[str, Any] = {}
        for key in pmt.to_python(pmt.dict_keys(obj)):
            key_symbol = pmt.string_to_symbol(key)
            value = pmt.dict_ref(obj, key_symbol, pmt.PMT_NIL)
            result[key] = pmt_to_python(value)
        return result
    if pmt.is_tuple(obj):
        return [pmt_to_python(pmt.tuple_ref(obj, i)) for i in range(pmt.tuple_length(obj))]
    try:
        return pmt.to_python(obj)
    except RuntimeError:
        return pmt.serialize_str(obj)


def extract_frame_fields(info: pmt.pmt_t) -> Dict[str, Any]:
    fields: Dict[str, Any] = {}

    for key in FRAME_INFO_KEYS:
        symbol = pmt.string_to_symbol(key)
        value = pmt.dict_ref(info, symbol, pmt.PMT_NIL)
        if value is pmt.PMT_NIL:
            continue
        fields[key] = pmt_to_python(value)

    extra_symbols = {
        "snr": pmt.string_to_symbol("snr"),
        "freq_offset": pmt.string_to_symbol("freq_offset"),
    }
    for name, symbol in extra_symbols.items():
        value = pmt.dict_ref(info, symbol, pmt.PMT_NIL)
        if value is not pmt.PMT_NIL:
            fields[name] = pmt_to_python(value)

    return fields


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
        "has_crc": match.group("crc").lower() == "true",
        "impl_head": match.group("impl").lower() == "true",
    }

    ldro_raw = match.group("ldro").lower()
    if ldro_raw in {"true", "false"}:
        params["ldro_mode"] = 1 if ldro_raw == "true" else 0
    else:
        try:
            params["ldro_mode"] = int(ldro_raw)
        except ValueError:
            params["ldro_mode"] = 2  # default to auto when inference fails

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
        crc_present = pmt.to_bool(crc_present_pmt) if not pmt.is_null(crc_present_pmt) and pmt.is_bool(crc_present_pmt) else None

        crc_valid_pmt = pmt.dict_ref(info, pmt.string_to_symbol("crc_valid"), pmt.PMT_NIL)
        crc_valid = pmt.to_bool(crc_valid_pmt) if not pmt.is_null(crc_valid_pmt) and pmt.is_bool(crc_valid_pmt) else None

        payload = payload_bytes[cursor:cursor + pay_len]
        cursor += pay_len

        tag_info = extract_frame_fields(info)
        if not tag_info:
            tag_info = {"raw": pmt_to_python(info)}

        frames.append(
            FrameResult(
                index=len(frames),
                payload=payload,
                crc_valid=crc_valid,
                has_crc=crc_present,
                message_text=None,
                tag_offset=int(tag.offset),
                tag_info=tag_info,
            )
        )

    return frames


def combine_messages(frames: List[FrameResult], messages: Sequence[bytes]) -> None:
    for frame, msg_bytes in zip(frames, messages):
        try:
            frame.message_text = msg_bytes.decode("utf-8")
        except UnicodeDecodeError:
            frame.message_text = msg_bytes.decode("latin-1", errors="replace")


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
    parser.add_argument("--limit", type=int, default=None, help="Process only the first N IQ samples")
    parser.add_argument("--print-tags", action="store_true", help="Dump full frame_info tag dictionaries")
    parser.add_argument("--dump-json", type=Path, help="Write detailed frame information to a JSON file")
    parser.add_argument("--show-messages", action="store_true", help="Print raw GNU Radio messages captured on the RX out port")
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

    iq = np.fromfile(file_path, dtype=np.complex64)
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
    )

    receiver.run()
    payload_bytes, tags, messages = receiver.results()
    frames = extract_frames(payload_bytes, tags)
    combine_messages(frames, messages)

    total_samples = len(iq)
    total_frames = len(frames)
    print(f"Decoded {total_frames} frame(s) from {total_samples} IQ samples")

    if args.show_messages and messages:
        print("Captured GNU Radio messages:")
        for idx, raw in enumerate(messages):
            print(f"  Message {idx}: {raw!r}")

    if not frames:
        print("No frames were decoded")
        if args.dump_json:
            dump_path = args.dump_json.expanduser()
            dump_path.parent.mkdir(parents=True, exist_ok=True)
            with dump_path.open("w", encoding="utf-8") as fh:
                json.dump(
                    {
                        "input_file": str(file_path),
                        "parameters": {
                            "sf": sf,
                            "bw": bw,
                            "samp_rate": samp_rate,
                            "cr": cr,
                            "ldro_mode": ldro_mode,
                            "has_crc": has_crc,
                            "impl_head": impl_head,
                        },
                        "frames": [],
                        "messages": [raw.decode("latin-1", errors="replace") for raw in messages],
                    },
                    fh,
                    indent=2,
                )
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
        if args.print_tags:
            print(f"  Tag offset: {frame.tag_offset}")
            print(f"  Tag info: {json.dumps(frame.tag_info, indent=2, sort_keys=True)}")
        
        # Debug: Show detailed payload analysis
        print(f"  Payload analysis:")
        print(f"    Length: {len(frame.payload)} bytes")
        print(f"    CRC valid: {frame.crc_valid}")
        if hasattr(frame, 'raw_payload'):
            print(f"    Raw payload: {' '.join(f'{b:02x}' for b in frame.raw_payload)}")
        print(f"    Dewhitened: {' '.join(f'{b:02x}' for b in frame.payload)}")

    if args.dump_json:
        dump_path = args.dump_json.expanduser()
        dump_path.parent.mkdir(parents=True, exist_ok=True)
        dump_payload = [
            {
                "index": frame.index,
                "payload_hex": frame.hex_payload(),
                "payload_text": frame.message_text,
                "payload_len": len(frame.payload),
                "crc_valid": frame.crc_valid,
                "has_crc": frame.has_crc,
                "tag_offset": frame.tag_offset,
                "tag_info": frame.tag_info,
            }
            for frame in frames
        ]
        with dump_path.open("w", encoding="utf-8") as fh:
            json.dump(
                {
                    "input_file": str(file_path),
                    "parameters": {
                        "sf": sf,
                        "bw": bw,
                        "samp_rate": samp_rate,
                        "cr": cr,
                        "ldro_mode": ldro_mode,
                        "has_crc": has_crc,
                        "impl_head": impl_head,
                    },
                    "frames": dump_payload,
                    "messages": [raw.decode("latin-1", errors="replace") for raw in messages],
                    "debug_info": {
                        "total_samples": len(samples),
                        "frame_count": len(messages),
                        "sample_rate": samp_rate,
                        "symbol_duration": (2**sf) / bw,
                        "frame_duration_samples": int(samp_rate * (2**sf) / bw),
                    }
                },
                fh,
                indent=2,
            )


if __name__ == "__main__":
    main()
