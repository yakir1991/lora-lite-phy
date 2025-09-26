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

import os
import sys
import glob
import importlib
import numpy as np
import pmt
from gnuradio import blocks, gr

def _attempt_import_lora_sdr(user_paths: list[str] | None = None):
    """Try importing gnuradio.lora_sdr with optional additional search paths.

    This allows using the script without a full 'make install' by pointing to a
    build directory containing the Python bindings (e.g. build_release/python).
    """
    tried: list[tuple[str,str]] = []

    def _try(msg: str):
        try:
            mod = importlib.import_module("gnuradio.lora_sdr")
            return mod, msg
        except Exception as e:  # noqa
            tried.append((msg, repr(e)))
            return None, msg

    # First: plain import
    mod, _ = _try("sys.path baseline")
    if mod:
        return mod

    # Second: user provided paths
    for up in user_paths or []:
        if not up:
            continue
        if os.path.isdir(up) and up not in sys.path:
            sys.path.insert(0, up)
        mod, _ = _try(f"user:{up}")
        if mod:
            return mod

    # Third: heuristic search relative to this script (common build dirs)
    script_root = Path(__file__).resolve().parent.parent  # external/gr_lora_sdr/scripts -> external/gr_lora_sdr
    candidate_globs = [
        "build*/python",  # in-tree build pattern
        "build*/lib*",     # some CMake layouts
        "../build*/python",  # if script copied elsewhere
    ]
    for pat in candidate_globs:
        for cand in script_root.glob(pat):
            if not cand.is_dir():
                continue
            if str(cand) not in sys.path:
                sys.path.insert(0, str(cand))
            mod, _ = _try(f"auto:{cand}")
            if mod:
                return mod

    # Fourth: scan CONDA_PREFIX if set
    cprefix = os.environ.get("CONDA_PREFIX")
    if cprefix:
        for sp in glob.glob(os.path.join(cprefix, "lib", "python*", "site-packages")):
            if sp not in sys.path:
                sys.path.insert(0, sp)
            mod, _ = _try(f"conda:{sp}")
            if mod:
                return mod

    # If still not found, raise a detailed error
    details = "\n".join(f"  - {ctx}: {err}" for ctx, err in tried)
    raise ImportError(
        "Could not import gnuradio.lora_sdr.\n"
        "Tried contexts:\n" + details + "\n\n"
        "Fix options:\n"
        "  1) Build & install:  cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release && \n"
        "                       cmake --build build_release -j$(nproc) && cmake --install build_release\n"
        "  2) Or pass --module-path pointing to build_release/python\n"
        "  3) Ensure PYTHONPATH includes the directory containing 'gnuradio/lora_sdr/__init__.py'\n"
    )

def _prepend_module_path(path: str | None):
    if path and os.path.isdir(path) and path not in sys.path:
        sys.path.insert(0, path)

# Placeholder; real import executed after argparse (so we can parse --module-path)
lora_sdr = None  # type: ignore


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
        dump_stages: bool = False,
        max_dump: Optional[int] = None,
    ) -> None:
        super().__init__("offline_lora_receiver", catch_exceptions=True)

        center_freq = int(round(center_freq))
        bw = int(round(bw))
        samp_rate = int(round(samp_rate))

        self.src = blocks.vector_source_c(list(iq_samples), False)
        
        # Add debug print for input parameters
        print(f"DEBUG: Setting up receiver with:")
        print(f"  - SF: {sf}, CR: {cr}, BW: {bw} Hz, Sample rate: {samp_rate} Hz")
        print(f"  - Sync words: {sync_word}, CRC: {has_crc}, Implicit header: {impl_head}")
        print(f"  - LDRO mode: {ldro_mode}, Soft decoding: {soft_decoding}")
        print(f"  - Input samples: {len(iq_samples)}")
        
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

        # Optional taps for debugging (access internal blocks of hierarchical rx)
        self._dump_stages = dump_stages
        self._max_dump = max_dump
        self._stage_sinks: Dict[str, object] = {}
        if dump_stages:
            try:
                # frame_sync output (complex)
                self._stage_sinks["frame_sync_c"] = blocks.vector_sink_c()
                self.connect(self.rx.lora_sdr_frame_sync_0, self._stage_sinks["frame_sync_c"])  # type: ignore[attr-defined]
                # tap input to fft_demod (same as frame_sync out for current hierarchy) for clarity
                self._stage_sinks["fft_in_c"] = blocks.vector_sink_c()
                self.connect(self.rx.lora_sdr_frame_sync_0, self._stage_sinks["fft_in_c"])  # type: ignore[attr-defined]
                # fft_demod output:
                #   In hard-decision mode (soft_decoding=False) the block outputs uint16_t symbol values.
                #   Use a short sink (vector_sink_s) to match itemsize (2 bytes) and avoid mismatch warnings.
                self._stage_sinks["fft_demod_s"] = blocks.vector_sink_s()
                self.connect(self.rx.lora_sdr_fft_demod_0, self._stage_sinks["fft_demod_s"])  # type: ignore[attr-defined]
                # gray_mapping output (also uint16_t symbols)
                self._stage_sinks["gray_map_s"] = blocks.vector_sink_s()
                self.connect(self.rx.lora_sdr_gray_mapping_0, self._stage_sinks["gray_map_s"])  # type: ignore[attr-defined]
                # deinterleaver output (bytes: sf_app or codeword length per batch)
                self._stage_sinks["deinterleave_b"] = blocks.vector_sink_b()
                self.connect(self.rx.lora_sdr_deinterleaver_0, self._stage_sinks["deinterleave_b"])  # type: ignore[attr-defined]
                # hamming_dec output (decoded 4-bit nibbles packed into bytes)
                self._stage_sinks["hamming_b"] = blocks.vector_sink_b()
                self.connect(self.rx.lora_sdr_hamming_dec_0, self._stage_sinks["hamming_b"])  # type: ignore[attr-defined]
                # header_decoder output (whitened payload bytes entering dewhitening)
                self._stage_sinks["header_out_b"] = blocks.vector_sink_b()
                self.connect(self.rx.lora_sdr_header_decoder_0, self._stage_sinks["header_out_b"])  # type: ignore[attr-defined]
            except Exception as e:  # pragma: no cover - defensive
                print(f"[warn] failed attaching stage taps: {e}")
    
    def print_stage_debug(self) -> None:
        """Print debug information about all processing stages"""
        if not self._dump_stages:
            return
            
        print("\n" + "="*60)
        print("DEBUG: Processing stages information")
        print("="*60)
        
        for name, sink in self._stage_sinks.items():
            try:
                data = list(sink.data())  # type: ignore[attr-defined]
                if not data:
                    print(f"{name:20s}: No data")
                    continue
                    
                if name.endswith("_c"):  # Complex data
                    print(f"{name:20s}: {len(data)} complex samples")
                    if len(data) > 0:
                        print(f"{'':20s}  First: {data[0]:.6f}")
                        print(f"{'':20s}  Last:  {data[-1]:.6f}")
                        # Show magnitude statistics
                        mags = [abs(x) for x in data[:100]]  # First 100 samples
                        if mags:
                            print(f"{'':20s}  Mag avg: {sum(mags)/len(mags):.6f}")
                            
                elif name.endswith("_s"):  # Symbol data (uint16)
                    print(f"{name:20s}: {len(data)} symbols")
                    if len(data) > 0:
                        symbols_preview = data[:min(20, len(data))]
                        print(f"{'':20s}  Symbols: {symbols_preview}")
                        
                elif name.endswith("_b"):  # Byte data
                    print(f"{name:20s}: {len(data)} bytes")
                    if len(data) > 0:
                        bytes_preview = data[:min(32, len(data))]
                        hex_str = ' '.join(f'{b:02x}' for b in bytes_preview)
                        print(f"{'':20s}  Hex: {hex_str}")
                        # Try to decode as ASCII
                        try:
                            ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in bytes_preview)
                            print(f"{'':20s}  ASCII: {ascii_str}")
                        except:
                            pass
                else:
                    print(f"{name:20s}: {len(data)} items: {str(data)[:100]}")
                    
            except Exception as e:
                print(f"{name:20s}: Error reading data: {e}")
        
        print("="*60)

    def stage_data(self) -> Dict[str, object]:
        if not self._dump_stages:
            return {}
        out: Dict[str, object] = {}
        frame_tags: List[gr.tag_t] = []  # collected candidate frame_info tags
        for name, sink in self._stage_sinks.items():
            try:
                data = list(sink.data())  # type: ignore[attr-defined]
            except Exception:
                continue
            if self._max_dump is not None and len(data) > self._max_dump:
                data = data[: self._max_dump]
            # Convert complex to separate I/Q lists for JSON friendliness
            if name.endswith("_c"):
                out[name] = {"i": [float(v.real) for v in data], "q": [float(v.imag) for v in data]}
            else:
                # For fft_demod we rename for clarity
                if name == "fft_demod_s":
                    out["fft_demod_sym"] = data
                elif name == "gray_map_s":
                    out["gray_demap_sym"] = data
                else:
                    out[name] = data
            # Attempt to collect any frame_info tags directly from the vector sink (more reliable than block get_tags calls)
            # vector_sink_* in GNU Radio stores tags; we inspect them if available
            try:  # pragma: no cover - best effort
                if hasattr(sink, "tags"):
                    for t in sink.tags():  # type: ignore[attr-defined]
                        try:
                            if pmt.symbol_to_string(t.key) == "frame_info":
                                frame_tags.append(t)
                        except Exception:
                            continue
            except Exception:
                pass
        # If we collected any frame_info tags, serialize the first one
        if frame_tags:
            try:
                val = frame_tags[0].value
                def _get(key: str):  # helper extracting a field or None
                    err = pmt.intern("error")
                    try:
                        return pmt.to_python(pmt.dict_ref(val, pmt.intern(key), err))
                    except Exception:
                        return None
                out["frame_info"] = {
                    "cfo_int": _get("cfo_int"),
                    "cfo_frac": _get("cfo_frac"),
                    "sf": _get("sf"),
                    "is_header": _get("is_header"),
                    "cr": _get("cr"),
                    "ldro": _get("ldro"),
                    "symb_numb": _get("symb_numb"),
                }
            except Exception:
                pass
        return out

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
    parser.add_argument("--module-path", dest="module_path", type=str, default=None,
                        help="Optional path to add to sys.path to locate gnuradio.lora_sdr (e.g. build_release/python)")
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
    parser.add_argument("--dump-stages", action="store_true", help="Tap and dump intermediate RX stages")
    parser.add_argument("--dump-json", type=Path, default=None, help="Write stage + frame info JSON to this file")
    parser.add_argument("--max-dump", type=int, default=4096, help="Limit number of samples/bytes per stage dump")
    parser.set_defaults(has_crc=None, impl_head=None)

    args = parser.parse_args()

    # Late import handling
    _prepend_module_path(args.module_path)
    global lora_sdr  # noqa: PLW0603
    lora_sdr = _attempt_import_lora_sdr([args.module_path] if args.module_path else None)

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
        dump_stages=args.dump_stages,
        max_dump=args.max_dump,
    )

    receiver.run()
    payload_bytes, tags, messages = receiver.results()
    
    # Add debug information about stages
    if args.dump_stages:
        receiver.print_stage_debug()
    
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

    if args.dump_json:
        import json
        # Derive raw 4-bit data stream (post-Hamming decode) from existing stage array if present.
        post_hamming_bits: list[int] = []
        try:
            stages = receiver.stage_data()  # re-fetch (already done above in creation of 'dump', but safe)
        except Exception:
            stages = {}
        # hamming_b consists of decoded 4-bit nibbles (each stored in a byte 0..15)
        if isinstance(stages.get("hamming_b"), list):
            for v in stages["hamming_b"]:  # type: ignore[index]
                if isinstance(v, (int, float)):
                    nib = int(v) & 0xF
                    # LSB-first bit order (b0..b3) to align with whitening step expectations in C++ side
                    for b in range(4):
                        post_hamming_bits.append((nib >> b) & 1)
        dump = {
            "params": {
                "sf": sf,
                "bw": bw,
                "samp_rate": samp_rate,
                "cr": cr,
                "has_crc": has_crc,
                "impl_head": impl_head,
                "ldro_mode": ldro_mode,
                "pay_len": args.pay_len,
            },
            "frames": [
                {
                    "index": f.index,
                    "payload_hex": f.hex_payload(),
                    "crc_valid": f.crc_valid,
                    "has_crc": f.has_crc,
                    "text": f.message_text,
                }
                for f in frames
            ],
            "stages": {**receiver.stage_data(), "post_hamming_bits_b": post_hamming_bits},
        }
        try:
            args.dump_json.parent.mkdir(parents=True, exist_ok=True)
            args.dump_json.write_text(json.dumps(dump, indent=2))
            print(f"[info] wrote stage dump -> {args.dump_json}")
        except Exception as e:  # pragma: no cover
            print(f"[warn] failed writing dump json: {e}")


if __name__ == "__main__":
    main()
