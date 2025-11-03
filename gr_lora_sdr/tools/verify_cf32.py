#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0
"""
Quick verification helper that decodes a captured LoRa IQ stream using the
reference GNU Radio blocks and prints the recovered payloads.

Example:
    PYTHONPATH=$PWD/gr_lora_sdr/install/lib/python3.12/site-packages \\
    LD_LIBRARY_PATH=$PWD/gr_lora_sdr/install/lib \\
    conda run -n gr310 python gr_lora_sdr/tools/verify_cf32.py \\
        --input ../data/generated/tx_rx_simulation.cf32
"""

from __future__ import annotations

import argparse
from pathlib import Path

from gnuradio import blocks, gr
import gnuradio.lora_sdr as lora_sdr


def _build_flowgraph(
    input_path: Path,
    sf: int,
    bw: int,
    samp_rate: int,
    cr: int,
    payload_len: int,
    sync_word: int,
    preamb_len: int,
    impl_header: bool,
    has_crc: bool,
    ldro: bool,
) -> tuple[gr.top_block, blocks.vector_sink_b]:
    """Instantiate the receive chain used by the verification helper."""

    tb = gr.top_block()
    vector_sink = blocks.vector_sink_b()

    file_source = blocks.file_source(gr.sizeof_gr_complex, str(input_path), False)
    frame_sync = lora_sdr.frame_sync(
        int(868.1e6), bw, sf, impl_header, [sync_word], int(samp_rate / bw), preamb_len
    )
    fft_demod = lora_sdr.fft_demod(False, False)
    gray_mapping = lora_sdr.gray_mapping(False)
    deinterleaver = lora_sdr.deinterleaver(False)
    hamming_dec = lora_sdr.hamming_dec(False)
    header_decoder = lora_sdr.header_decoder(
        impl_header, cr, payload_len, has_crc, ldro, True
    )
    dewhitening = lora_sdr.dewhitening()
    crc_verif = lora_sdr.crc_verif(1, False)

    tb.msg_connect((header_decoder, "frame_info"), (frame_sync, "frame_info"))
    tb.connect(
        file_source,
        frame_sync,
        fft_demod,
        gray_mapping,
        deinterleaver,
        hamming_dec,
        header_decoder,
        dewhitening,
        crc_verif,
        vector_sink,
    )

    return tb, vector_sink


def decode_file(args: argparse.Namespace) -> list[bytes]:
    tb, sink = _build_flowgraph(
        input_path=args.input,
        sf=args.sf,
        bw=args.bw,
        samp_rate=args.sample_rate,
        cr=args.cr,
        payload_len=args.payload_len,
        sync_word=args.sync_word,
        preamb_len=args.preamble_len,
        impl_header=args.implicit_header,
        has_crc=not args.no_crc,
        ldro=args.ldro,
    )
    tb.run()

    raw = bytes(sink.data())
    step = args.payload_len
    return [raw[i : i + step] for i in range(0, len(raw), step) if len(raw[i:]) >= step]


def main() -> None:
    parser = argparse.ArgumentParser(description="Decode a LoRa CF32 capture")
    parser.add_argument("--input", type=Path, required=True, help="Path to cf32 capture")
    parser.add_argument("--sf", type=int, default=7, help="Spreading factor (default: 7)")
    parser.add_argument("--bw", type=int, default=125000, help="Bandwidth in Hz")
    parser.add_argument(
        "--sample-rate", type=int, default=500000, help="Sample rate of capture"
    )
    parser.add_argument(
        "--cr",
        type=int,
        default=2,
        help="Hamming code rate (0->4/4 ... 4->4/8). Default matches example flowgraph.",
    )
    parser.add_argument(
        "--payload-len",
        type=int,
        default=16,
        help="Payload bytes per frame (default: 16).",
    )
    parser.add_argument(
        "--sync-word",
        type=lambda x: int(x, 0),
        default=0x12,
        help="Sync word in hex/decimal (default: 0x12).",
    )
    parser.add_argument(
        "--preamble-len",
        type=int,
        default=8,
        help="Preamble upchirp repetitions (default: 8).",
    )
    parser.add_argument(
        "--implicit-header",
        action="store_true",
        help="Decode assuming implicit header mode (default: explicit).",
    )
    parser.add_argument(
        "--no-crc",
        action="store_true",
        help="Skip CRC verification if frames omitted CRC bytes.",
    )
    parser.add_argument(
        "--ldro",
        action="store_true",
        help="Enable low data rate optimisation for the decode chain.",
    )
    parser.add_argument(
        "--max-frames",
        type=int,
        default=10,
        help="Limit frames printed to avoid overwhelming stdout (default: 10).",
    )
    args = parser.parse_args()

    payloads = decode_file(args)
    total = len(payloads)
    unique = sorted(set(payloads))

    print(f"Decoded frames: {total}")
    print(f"Unique payloads: {len(unique)}")
    for idx, frame in enumerate(unique[: args.max_frames], start=1):
        printable = frame.decode("ascii", "replace")
        print(f"[{idx:02d}] {printable} ({frame.hex()})")

    if len(unique) > args.max_frames:
        print(f"... truncated list (use --max-frames to show more).")


if __name__ == "__main__":
    main()
