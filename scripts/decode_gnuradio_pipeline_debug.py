#!/usr/bin/env python3
"""Enhanced debug version of decode_offline_recording.py with detailed pipeline analysis.

This script provides detailed debug information for every stage of the GNU Radio LoRa pipeline
to enable 1:1 comparison with the new C++ pipeline.
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


class DebugMessageCollector(gr.sync_block):
    """Enhanced message collector with detailed debug information."""

    def __init__(self):
        super().__init__(name="debug_message_collector", in_sig=None, out_sig=None)
        self.message_port_register_in(pmt.intern("in"))
        self.set_msg_handler(pmt.intern("in"), self._handle_msg)
        self.messages: List[bytes] = []
        self.debug_info: List[Dict[str, Any]] = []

    def _handle_msg(self, msg: pmt.pmt_t) -> None:
        debug_info = {
            "timestamp": len(self.messages),
            "msg_type": str(type(msg)),
            "pmt_type": str(type(msg))
        }
        
        if pmt.is_symbol(msg):
            text = pmt.symbol_to_string(msg)
            self.messages.append(text.encode("latin-1", errors="ignore"))
            debug_info["content"] = text
            debug_info["content_type"] = "symbol"
        elif pmt.is_u8vector(msg):
            data = bytes(pmt.u8vector_elements(msg))
            self.messages.append(data)
            debug_info["content"] = data.hex()
            debug_info["content_type"] = "u8vector"
        else:
            serialized = pmt.serialize_str(msg)
            self.messages.append(serialized)
            debug_info["content"] = serialized.hex()
            debug_info["content_type"] = "serialized"
        
        self.debug_info.append(debug_info)
        print(f"DEBUG: Message {len(self.messages)-1}: {debug_info}")

    def work(self, input_items, output_items):  # pragma: no cover - message only block
        return 0


class DebugTagCollector(gr.sync_block):
    """Collect and analyze tags from the pipeline."""

    def __init__(self):
        super().__init__(name="debug_tag_collector", in_sig=[np.complex64], out_sig=[np.complex64])
        self.tags_collected: List[Dict[str, Any]] = []
        self.symbol_count = 0

    def work(self, input_items, output_items):
        # Copy input to output
        output_items[0][:] = input_items[0]
        
        # Collect tags
        tags = self.get_tags_in_window(0, 0, len(input_items[0]))
        for tag in tags:
            tag_key = pmt.symbol_to_string(tag.key)
            tag_value = pmt_to_python(tag.value)
            
            tag_info = {
                "offset": tag.offset,
                "key": tag_key,
                "value": tag_value,
                "srcid": tag.srcid,
                "symbol_index": self.symbol_count
            }
            self.tags_collected.append(tag_info)
            
            # Enhanced debug output for symbols
            if tag_key in ["frame_info", "symb_numb", "symbol"]:
                print(f"DEBUG: Symbol[{self.symbol_count}] at offset {tag.offset}: {tag_key} = {tag_value}")
                self.symbol_count += 1
            else:
                print(f"DEBUG: Tag at offset {tag.offset}: {tag_info}")
        
        return len(input_items[0])


def pmt_to_python(obj: pmt.pmt_t) -> Any:
    """Convert PMT object to Python object."""
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
        return pmt.serialize_str(obj).decode("utf-8", errors="replace")
    except Exception:
        return str(obj)


class DebugOfflineLoraReceiver(gr.top_block):
    """Enhanced receiver with detailed debug information."""

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
        super().__init__("debug_offline_lora_receiver", catch_exceptions=True)

        print(f"DEBUG: Initializing receiver with:")
        print(f"  Sample rate: {samp_rate}")
        print(f"  Bandwidth: {bw}")
        print(f"  Spreading factor: {sf}")
        print(f"  Coding rate: {cr}")
        print(f"  Has CRC: {has_crc}")
        print(f"  Implicit header: {impl_head}")
        print(f"  LDRO mode: {ldro_mode}")
        print(f"  Sync word: {sync_word}")
        print(f"  Payload length: {pay_len}")
        print(f"  Soft decoding: {soft_decoding}")
        print(f"  Center frequency: {center_freq}")
        print(f"  IQ samples: {len(iq_samples)}")

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
        
        # Add debug output for GNU Radio parameters
        print(f"DEBUG: GNU Radio LoRa RX parameters:")
        print(f"  Center freq: {center_freq}")
        print(f"  Bandwidth: {bw}")
        print(f"  Sample rate: {samp_rate}")
        print(f"  Spreading factor: {sf}")
        print(f"  Coding rate: {cr}")
        print(f"  Sync word: {sync_word}")
        print(f"  Has CRC: {has_crc}")
        print(f"  Implicit header: {impl_head}")
        self.payload_sink = blocks.vector_sink_b()
        self.msg_collector = DebugMessageCollector()
        self.tag_collector = DebugTagCollector()

        # Connect the pipeline
        self.connect(self.src, self.tag_collector)
        self.connect(self.tag_collector, self.rx)
        self.connect(self.rx, self.payload_sink)
        self.msg_connect((self.rx, "out"), (self.msg_collector, "in"))

    def results(self) -> Tuple[bytes, Iterable[gr.tag_t], List[bytes], List[Dict[str, Any]], List[Dict[str, Any]]]:
        payload_bytes = bytes(self.payload_sink.data())
        tags = list(self.payload_sink.tags())
        messages = list(self.msg_collector.messages)
        debug_messages = list(self.msg_collector.debug_info)
        debug_tags = list(self.tag_collector.tags_collected)
        
        print(f"DEBUG: Pipeline results:")
        print(f"  Payload bytes: {len(payload_bytes)} bytes")
        print(f"  Tags collected: {len(tags)}")
        print(f"  Messages collected: {len(messages)}")
        print(f"  Debug messages: {len(debug_messages)}")
        print(f"  Debug tags: {len(debug_tags)}")
        
        return payload_bytes, tags, messages, debug_messages, debug_tags


def parse_frame_tags(tags: Iterable[gr.tag_t], messages: List[bytes]) -> List[FrameResult]:
    """Parse frame information from tags and messages."""
    frames: List[FrameResult] = []
    frame_tags: List[gr.tag_t] = []
    
    # Collect frame_info tags
    for tag in tags:
        if pmt.symbol_to_string(tag.key) == "frame_info":
            frame_tags.append(tag)
    
    print(f"DEBUG: Found {len(frame_tags)} frame_info tags")
    
    # Process each frame
    for i, tag in enumerate(frame_tags):
        frame_info = pmt_to_python(tag.value)
        print(f"DEBUG: Frame {i} info: {frame_info}")
        
        # Extract payload from messages
        payload = b""
        if i < len(messages):
            payload = messages[i]
        
        # Extract CRC info - handle both dict and list formats
        crc_valid = None
        has_crc = False
        
        if isinstance(frame_info, dict):
            crc_valid = frame_info.get("crc_valid")
            has_crc = frame_info.get("crc", 0) == 1
        elif isinstance(frame_info, list):
            # Handle nested list format
            for item in frame_info:
                if isinstance(item, list) and len(item) == 2:
                    key, value = item
                    if key == "crc_valid":
                        crc_valid = value
                    elif key == "crc":
                        has_crc = value == 1
        
        # Try to decode as text
        message_text = None
        try:
            message_text = payload.decode("utf-8", errors="replace")
        except Exception:
            pass
        
        frame = FrameResult(
            index=i,
            payload=payload,
            crc_valid=crc_valid,
            has_crc=has_crc,
            message_text=message_text,
            tag_offset=tag.offset,
            tag_info=frame_info,
        )
        frames.append(frame)
        
        print(f"DEBUG: Frame {i} details:")
        print(f"  Payload: {frame.hex_payload()}")
        print(f"  Text: {message_text}")
        print(f"  CRC valid: {crc_valid}")
        print(f"  Has CRC: {has_crc}")
        print(f"  Tag offset: {tag.offset}")
    
    return frames


def load_iq_samples(file_path: Path) -> np.ndarray:
    """Load IQ samples from file."""
    print(f"DEBUG: Loading IQ samples from {file_path}")
    
    if file_path.suffix == ".bin":
        # Binary format
        samples = np.fromfile(file_path, dtype=np.complex64)
    elif file_path.suffix == ".unknown":
        # Unknown format - try as complex32
        samples = np.fromfile(file_path, dtype=np.complex64)
    else:
        raise ValueError(f"Unsupported file format: {file_path.suffix}")
    
    print(f"DEBUG: Loaded {len(samples)} complex samples")
    print(f"DEBUG: Sample range: {np.min(samples.real):.6f} to {np.max(samples.real):.6f} (real)")
    print(f"DEBUG: Sample range: {np.min(samples.imag):.6f} to {np.max(samples.imag):.6f} (imag)")
    
    return samples


def main() -> None:
    parser = argparse.ArgumentParser(description="Debug LoRa offline recording decoder")
    parser.add_argument("file_path", type=Path, help="Path to IQ samples file")
    parser.add_argument("--sf", type=int, required=True, help="Spreading factor")
    parser.add_argument("--bw", type=float, required=True, help="Bandwidth")
    parser.add_argument("--samp-rate", type=float, required=True, help="Sample rate")
    parser.add_argument("--cr", type=int, required=True, help="Coding rate")
    parser.add_argument("--sync-word", type=str, default="0x34", help="Sync word (hex)")
    parser.add_argument("--ldro-mode", type=int, default=0, help="LDRO mode")
    parser.add_argument("--has-crc", action="store_true", help="Has CRC")
    parser.add_argument("--impl-head", action="store_true", help="Implicit header")
    parser.add_argument("--pay-len", type=int, help="Payload length")
    parser.add_argument("--soft-decoding", action="store_true", help="Soft decoding")
    parser.add_argument("--center-freq", type=float, default=868.1e6, help="Center frequency")
    parser.add_argument("--print-tags", action="store_true", help="Print tag information")
    parser.add_argument("--print-symbols", action="store_true", help="Print symbol information")
    parser.add_argument("--dump-json", type=Path, help="Dump results to JSON file")

    args = parser.parse_args()

    # Parse sync word
    sync_word_hex = args.sync_word
    if sync_word_hex.startswith("0x"):
        sync_word_hex = sync_word_hex[2:]
    sync_word = [int(sync_word_hex[i:i+2], 16) for i in range(0, len(sync_word_hex), 2)]

    # Load samples
    samples = load_iq_samples(args.file_path)

    # Create receiver
    receiver = DebugOfflineLoraReceiver(
        iq_samples=samples,
        samp_rate=args.samp_rate,
        bw=args.bw,
        sf=args.sf,
        cr=args.cr,
        has_crc=args.has_crc,
        impl_head=args.impl_head,
        ldro_mode=args.ldro_mode,
        sync_word=sync_word,
        pay_len=args.pay_len or 0,
        soft_decoding=args.soft_decoding,
        center_freq=args.center_freq,
        print_header=True,
        print_payload=True,
    )

    print("DEBUG: Starting GNU Radio flowgraph...")
    receiver.start()
    receiver.wait()
    print("DEBUG: GNU Radio flowgraph completed")

    # Get results
    payload_bytes, tags, messages, debug_messages, debug_tags = receiver.results()
    
    # Debug: Print all tags to see what we have
    print(f"DEBUG: All tags collected:")
    for i, tag in enumerate(tags):
        tag_key = pmt.symbol_to_string(tag.key)
        tag_value = pmt_to_python(tag.value)
        print(f"  Tag[{i}]: key='{tag_key}', value={tag_value}, offset={tag.offset}")

    # Parse frames
    frames = parse_frame_tags(tags, messages)

    print(f"\nDEBUG: Summary:")
    print(f"Decoded {len(frames)} frame(s) from {len(samples)} IQ samples")
    
    for frame in frames:
        crc_state = "valid" if frame.crc_valid else "invalid" if frame.crc_valid is False else "unknown"
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
        print(f"    Dewhitened: {frame.hex_payload()}")

    # Print symbol information if requested
    if args.print_symbols:
        print(f"\nDEBUG: Symbol Information:")
        symbol_tags = [tag for tag in receiver.tag_collector.tags_collected 
                      if tag["key"] in ["frame_info", "symb_numb", "symbol"]]
        print(f"Captured {len(symbol_tags)} symbol-related events")
        for i, symbol in enumerate(symbol_tags[:50]):  # Show first 50
            print(f"  Symbol[{i}]: {symbol}")

    # Dump to JSON if requested
    if args.dump_json:
        dump_path = args.dump_json.expanduser()
        dump_path.parent.mkdir(parents=True, exist_ok=True)
        dump_payload = [
            {
                "index": frame.index,
                "payload_hex": frame.hex_payload(),
                "payload_text": frame.message_text,
                "crc_valid": frame.crc_valid,
                "has_crc": frame.has_crc,
                "tag_offset": frame.tag_offset,
                "tag_info": frame.tag_info,
            }
            for frame in frames
        ]
        
        debug_data = {
            "input_file": str(args.file_path),
            "parameters": {
                "sf": args.sf,
                "bw": args.bw,
                "samp_rate": args.samp_rate,
                "cr": args.cr,
                "ldro_mode": args.ldro_mode,
                "has_crc": args.has_crc,
                "impl_head": args.impl_head,
                "sync_word": args.sync_word,
                "pay_len": args.pay_len,
                "soft_decoding": args.soft_decoding,
                "center_freq": args.center_freq,
            },
            "frames": dump_payload,
            "messages": [raw.decode("latin-1", errors="replace") for raw in messages],
            "debug_info": {
                "total_samples": len(samples),
                "frame_count": len(frames),
                "sample_rate": args.samp_rate,
                "symbol_duration": (2**args.sf) / args.bw,
                "frame_duration_samples": int(args.samp_rate * (2**args.sf) / args.bw),
                "debug_messages": debug_messages,
                "debug_tags": debug_tags,
            }
        }
        
        with dump_path.open("w", encoding="utf-8") as fh:
            json.dump(debug_data, fh, indent=2)
            
    # Print symbol information for debugging
    print(f"\nDEBUG: Symbol analysis:")
    payload_sink_data = bytes(receiver.payload_sink.data())
    print(f"Total payload bytes collected: {len(payload_sink_data)}")
    print(f"Payload bytes (first 50): {' '.join(f'{b:02x}' for b in payload_sink_data[:50])}")
    
    # Try to decode as "Hello New Pipeline" 
    if len(payload_sink_data) >= 18:
        try:
            text = payload_sink_data[:18].decode('latin-1')
            print(f"First 18 bytes as text: '{text}'")
        except:
            print(f"First 18 bytes: {' '.join(f'{b:02x}' for b in payload_sink_data[:18])}")
    
    # Print frame information
    print(f"\nFrame analysis:")
    for i, frame in enumerate(frames[:3]):  # Print first 3 frames
        print(f"Frame {i}: {frame}")
    
    # Print tag information
    print(f"\nTag analysis (first 20 tags):")
    for i, tag in enumerate(receiver.tag_collector.tags_collected[:20]):
        print(f"Tag[{i}]: key={tag['key']}, offset={tag['offset']}, value={tag.get('value', 'N/A')}")


if __name__ == "__main__":
    main()
