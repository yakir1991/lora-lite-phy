#!/usr/bin/env python3

"""Run GNU Radio's LoRa reference flowgraph and capture intermediate traces.

Important: activate the GNU Radio environment first (``conda activate gr310``)
so the flowgraph modules import correctly.

This helper drives `external/gr_lora_sdr/examples/tx_rx_simulation.py` with
explicit environment variables so the instrumented deinterleaver and Hamming
decoder dump their internal matrices. It produces JSONL files per spreading
factor that we can feed into comparison tooling against our C++ pipeline.
"""

from __future__ import annotations

import argparse
import os
import time
from pathlib import Path
from typing import List, Sequence


# The simulation lives in the external GNU Radio module. We add the parent
# directory to sys.path so Python can import it without needing an installed
# package.
import sys

REPO_ROOT = Path(__file__).resolve().parents[1]
GR_EXAMPLE_DIR = REPO_ROOT / "external" / "gr_lora_sdr" / "examples"
GR_BUILD_PYTHON = REPO_ROOT / "external" / "gr_lora_sdr" / "build" / "python"

if str(GR_EXAMPLE_DIR) not in sys.path:
    sys.path.insert(0, str(GR_EXAMPLE_DIR))

if GR_BUILD_PYTHON.exists():
    sys.path.insert(0, str(GR_BUILD_PYTHON))

LOCAL_PREFIX = os.environ.get("GR_LORA_SDR_PREFIX")
if LOCAL_PREFIX:
    python_dir = Path(LOCAL_PREFIX) / "lib" / f"python{sys.version_info.major}.{sys.version_info.minor}" / "site-packages" / "gnuradio"
    if python_dir.exists():
        try:
            import gnuradio  # type: ignore

            normalized = str(python_dir)
            updated = [normalized]
            for existing in gnuradio.__path__:
                if existing != normalized:
                    updated.append(existing)
            gnuradio.__path__ = updated  # type: ignore[attr-defined]
            import sys as _sys
            _sys.modules.pop("gnuradio.lora_sdr", None)
        except Exception as exc:  # pragma: no cover - diagnostic aid
            print(f"[collect_gr_lora_traces] warning: unable to reorder gnuradio path ({exc})")

        local_lib = Path(LOCAL_PREFIX) / "lib" / "libgnuradio-lora_sdr.so"
        if local_lib.exists():
            current = os.environ.get("LD_PRELOAD", "")
            if str(local_lib) not in current.split(":"):
                print(f"[collect_gr_lora_traces] note: set LD_PRELOAD={local_lib} to load the instrumented library")

from tx_rx_simulation import tx_rx_simulation  # type: ignore
from gnuradio import blocks, gr
import gnuradio.lora_sdr as lora_sdr  # type: ignore
import pmt


def parse_args(argv: List[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--sfs",
        type=int,
        nargs="+",
        default=[5, 6],
        help="Spreading factors to simulate (default: 5 6)",
    )
    parser.add_argument(
        "--cr",
        type=int,
        default=4,
        choices=[1, 2, 3, 4],
        help="Code rate denominator (4/x). Default: 4",
    )
    parser.add_argument(
        "--runtime",
        type=float,
        default=1.0,
        help="Seconds to let the flowgraph run before stopping (default: 1.0)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=REPO_ROOT / "results" / "gr_lora_traces",
        help="Where to write JSONL trace files",
    )
    parser.add_argument(
        "--overwrite",
        dest="overwrite",
        action="store_true",
        help="Overwrite existing trace files instead of appending",
    )
    parser.add_argument(
        "--no-overwrite",
        dest="overwrite",
        action="store_false",
        help="Append to existing trace files",
    )
    parser.add_argument(
        "--soft-decoding",
        dest="soft_decoding",
        action="store_true",
        help="Enable soft-decoding path inside GNU Radio blocks",
    )
    parser.add_argument(
        "--no-soft-decoding",
        dest="soft_decoding",
        action="store_false",
        help="Force hard-decoding path (default)",
    )
    parser.add_argument(
        "--driver",
        choices=["simulation", "synthetic"],
        default="synthetic",
        help="Choose between the full GNU Radio simulation or a synthetic header harness",
    )
    parser.add_argument(
        "--payload-len",
        type=int,
        default=16,
        help="Payload length used when synthesizing headers (default: 16)",
    )
    parser.add_argument(
        "--has-crc",
        dest="has_crc",
        action="store_true",
        help="Mark synthetic headers as having a payload CRC (default)",
    )
    parser.add_argument(
        "--no-has-crc",
        dest="has_crc",
        action="store_false",
        help="Mark synthetic headers as not having a payload CRC",
    )
    parser.set_defaults(soft_decoding=False, overwrite=True, has_crc=True)
    return parser.parse_args(argv)


def ensure_clean_file(path: Path, overwrite: bool) -> None:
    if path.exists() and overwrite:
        path.unlink()


def bits_from_uint(value: int, width: int) -> List[int]:
    return [(value >> (width - 1 - i)) & 1 for i in range(width)]


def bits_to_uint(bits: Sequence[int]) -> int:
    value = 0
    for bit in bits:
        value = (value << 1) | (bit & 1)
    return value


def build_header_nibbles(payload_len: int, cr: int, has_crc: bool) -> List[int]:
    hdr = [0] * 5
    hdr[0] = (payload_len >> 4) & 0x0F
    hdr[1] = payload_len & 0x0F
    hdr[2] = ((cr & 0x0F) << 1) | (1 if has_crc else 0)

    c4 = ((hdr[0] >> 3) & 1) ^ ((hdr[0] >> 2) & 1) ^ ((hdr[0] >> 1) & 1) ^ (hdr[0] & 1)
    c3 = ((hdr[0] >> 3) & 1) ^ ((hdr[1] >> 3) & 1) ^ ((hdr[1] >> 2) & 1) ^ ((hdr[1] >> 1) & 1) ^ (hdr[2] & 1)
    c2 = ((hdr[0] >> 2) & 1) ^ ((hdr[1] >> 3) & 1) ^ (hdr[1] & 1) ^ ((hdr[2] >> 3) & 1) ^ ((hdr[2] >> 1) & 1)
    c1 = ((hdr[0] >> 1) & 1) ^ ((hdr[1] >> 2) & 1) ^ (hdr[1] & 1) ^ ((hdr[2] >> 2) & 1) ^ ((hdr[2] >> 1) & 1) ^ (hdr[2] & 1)
    c0 = (hdr[0] & 1) ^ ((hdr[1] >> 1) & 1) ^ ((hdr[2] >> 3) & 1) ^ ((hdr[2] >> 2) & 1) ^ ((hdr[2] >> 1) & 1) ^ (hdr[2] & 1)

    hdr[3] = c4 & 1
    hdr[4] = ((c3 & 1) << 3) | ((c2 & 1) << 2) | ((c1 & 1) << 1) | (c0 & 1)
    return hdr


def hamming_encode_nibble(nibble: int, cr_app: int) -> int:
    b3 = (nibble >> 3) & 1
    b2 = (nibble >> 2) & 1
    b1 = (nibble >> 1) & 1
    b0 = nibble & 1

    if cr_app == 1:
        p4 = b0 ^ b1 ^ b2 ^ b3
        return (b3 << 4) | (b2 << 3) | (b1 << 2) | (b0 << 1) | p4

    p0 = b3 ^ b2 ^ b1
    p1 = b2 ^ b1 ^ b0
    p2 = b3 ^ b2 ^ b0
    p3 = b3 ^ b1 ^ b0
    raw = (
        (b3 << 7)
        | (b2 << 6)
        | (b1 << 5)
        | (b0 << 4)
        | (p0 << 3)
        | (p1 << 2)
        | (p2 << 1)
        | p3
    )
    shift = 4 - cr_app
    if shift > 0:
        raw >>= shift
    return raw


def synthesize_interleaver_inputs(sf: int, cr: int, has_crc: bool, payload_len: int) -> List[int]:
    sf_app = max(1, sf - 2)
    header_nibbles = build_header_nibbles(payload_len, cr, has_crc)

    codewords = []
    for idx in range(sf_app):
        codewords.append(hamming_encode_nibble(header_nibbles[idx], 4))

    cw_bin = [bits_from_uint(codewords[row], 8) for row in range(sf_app)]

    inter = []
    for col in range(8):
        column_bits = []
        for j in range(sf_app):
            src_row = (col - j - 1) % sf_app
            column_bits.append(cw_bin[src_row][col])
        inter.append(column_bits)

    return [bits_to_uint(bits) for bits in inter]


def run_flowgraph(sf: int,
                  cr: int,
                  runtime: float,
                  soft_decoding: bool,
                  output_dir: Path,
                  overwrite: bool,
                  driver: str,
                  payload_len: int,
                  has_crc: bool) -> None:
    deinterleaver_path = output_dir / f"sf{sf}_cr{cr}_deinterleaver.jsonl"
    hamming_path = output_dir / f"sf{sf}_cr{cr}_hamming.jsonl"

    ensure_clean_file(deinterleaver_path, overwrite)
    ensure_clean_file(hamming_path, overwrite)

    os.environ["GR_LORA_TRACE_DEINTERLEAVER"] = str(deinterleaver_path)
    os.environ["GR_LORA_TRACE_HAMMING_DEC"] = str(hamming_path)

    try:
        if driver == "synthetic":
            if soft_decoding:
                raise NotImplementedError("Synthetic driver currently supports hard decoding only")

            values = synthesize_interleaver_inputs(sf, cr, has_crc, payload_len)

            frame_dict = pmt.make_dict()
            frame_dict = pmt.dict_add(frame_dict, pmt.intern("is_header"), pmt.from_bool(True))
            frame_dict = pmt.dict_add(frame_dict, pmt.intern("sf"), pmt.from_long(sf))
            frame_dict = pmt.dict_add(frame_dict, pmt.intern("ldro"), pmt.from_bool(False))

            tag = gr.tag_t()
            tag.offset = 0
            tag.key = pmt.intern("frame_info")
            tag.value = frame_dict

            tb = gr.top_block()
            src = blocks.vector_source_s(values, repeat=False, tags=[tag])
            deinter = lora_sdr.deinterleaver(soft_decoding)
            hamming = lora_sdr.hamming_dec(soft_decoding)
            sink = blocks.null_sink(gr.sizeof_char)

            tb.connect(src, deinter)
            tb.connect(deinter, hamming)
            tb.connect(hamming, sink)
            tb.run()
        else:
            tb = tx_rx_simulation()
            tb.set_sf(sf)
            tb.set_cr(cr)
            tb.set_soft_decoding(soft_decoding)
            tb.set_SNRdB(100)
            tb.set_clk_offset(0)
            tb.set_ldro(False)

            tb.start()
            try:
                if hasattr(tb, "flowgraph_started"):
                    tb.flowgraph_started.wait(timeout=2.0)
                time.sleep(max(runtime, 0.1))
            finally:
                tb.stop()
                tb.wait()
    finally:
        os.environ.pop("GR_LORA_TRACE_DEINTERLEAVER", None)
        os.environ.pop("GR_LORA_TRACE_HAMMING_DEC", None)


def main(argv: List[str] | None = None) -> None:
    args = parse_args(argv)

    output_dir = args.output_dir if args.output_dir.is_absolute() else REPO_ROOT / args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    for sf in args.sfs:
        if sf < 5 or sf > 12:
            raise ValueError(f"LoRa spreading factor must be in [5, 12], received {sf}")
        run_flowgraph(
            sf=sf,
            cr=args.cr,
            runtime=args.runtime,
            soft_decoding=args.soft_decoding,
            output_dir=output_dir,
            overwrite=args.overwrite,
            driver=args.driver,
            payload_len=args.payload_len,
            has_crc=args.has_crc,
        )

    print(f"Traces written to {output_dir}")


if __name__ == "__main__":
    main()
