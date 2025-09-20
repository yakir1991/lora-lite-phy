#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Generate LoRa vectors with customizable parameters.
The generated files are compatible with decode_offline_recording.py.

Usage example:
    python3 generate_lora_vector.py --payload "hello world" --sf 7 --cr 2 --bw 125000
"""

from gnuradio import blocks
import pmt
from gnuradio import gr
import sys
import signal
import time
import argparse
import gnuradio.lora_sdr as lora_sdr
from pathlib import Path
import re


class LoRaVectorGenerator(gr.top_block):
    def __init__(
        self,
        payload_pmt,
        output_file: str,
        sf: int = 7,
        cr: int = 2,
        bw: int = 125000,
        samp_rate: int | None = None,
        ldro_mode: int = 0,
        has_crc: bool = True,
        impl_head: bool = False,
        sync_word: int = 0x12,
        strobe_ms: int = 500,
    ) -> None:
        gr.top_block.__init__(self, "LoRa Vector Generator", catch_exceptions=True)

        # Set default sample rate if not provided
        if samp_rate is None:
            samp_rate = bw * 4

        # Basic LoRa params
        self.bw = bw
        self.sf = sf
        self.samp_rate = samp_rate
        self.center_freq = 868.1e6
        self.clk_offset = 0
        self.SNRdB = -5

        # Blocks
        self.lora_tx_0 = lora_sdr.lora_sdr_lora_tx(
            bw=self.bw,
            cr=cr,
            has_crc=has_crc,
            impl_head=impl_head,
            samp_rate=self.samp_rate,
            sf=self.sf,
            ldro_mode=ldro_mode,
            frame_zero_padd=int(20 * (2 ** self.sf) * self.samp_rate / self.bw),
            sync_word=[sync_word],
        )

        # File sink to save the vector
        self.blocks_file_sink_0 = blocks.file_sink(
            gr.sizeof_gr_complex * 1, output_file, False
        )
        self.blocks_file_sink_0.set_unbuffered(False)

        # Message strobe to send payload periodically
        self.blocks_message_strobe_0_0 = blocks.message_strobe(payload_pmt, int(max(strobe_ms, 1)))

        # Connections
        self.msg_connect((self.blocks_message_strobe_0_0, 'strobe'), (self.lora_tx_0, 'in'))
        self.connect((self.lora_tx_0, 0), (self.blocks_file_sink_0, 0))


def generate_filename(
    payload: str,
    sf: int,
    cr: int,
    bw: int,
    samp_rate: int,
    ldro_mode: int,
    has_crc: bool,
    impl_head: bool,
) -> str:
    """Generate filename with lossless numeric parameters for decoder inference."""

    clean_payload = re.sub(r"[^0-9A-Za-z_-]+", "_", payload).strip("_")
    clean_payload = clean_payload[:20] if clean_payload else "payload"

    filename = (
        f"sps_{samp_rate}_"
        f"bw_{bw}_"
        f"sf_{sf}_"
        f"cr_{cr}_"
        f"ldro_{ldro_mode}_"
        f"crc_{str(has_crc).lower()}_"
        f"implheader_{str(impl_head).lower()}_{clean_payload}.unknown"
    )

    return filename


def main():
    parser = argparse.ArgumentParser(description="Generate LoRa vectors with customizable parameters")
    
    # Required parameters
    parser.add_argument("--payload", type=str, required=True, 
                       help="Payload message to transmit")
    
    # LoRa parameters
    parser.add_argument("--sf", type=int, default=7, choices=range(5, 13),
                       help="Spreading factor (5-12, default: 7)")
    parser.add_argument("--cr", type=int, default=2, choices=range(1, 5),
                       help="Coding rate (1-4, default: 2)")
    parser.add_argument("--bw", type=int, default=125000, 
                       help="Bandwidth in Hz (default: 125000)")
    parser.add_argument("--samp-rate", type=int, default=None,
                       help="Sampling rate in Hz (default: bw*4)")
    
    # Optional parameters
    parser.add_argument("--ldro-mode", type=int, default=0, choices=[0, 1, 2],
                       help="Low data rate optimization mode (0=off, 1=on, 2=auto, default: 0)")
    parser.add_argument("--no-crc", action="store_true",
                       help="Disable CRC (default: CRC enabled)")
    parser.add_argument("--impl-header", action="store_true",
                       help="Use implicit header (default: explicit header)")
    parser.add_argument("--sync-word", type=lambda x: int(x, 0), default=0x12,
                       help="Sync word as hex (default: 0x12)")
    parser.add_argument("--duration", type=int, default=5,
                       help="Generation duration in seconds (default: 5)")
    parser.add_argument("--strobe-ms", type=int, default=500,
                       help="Interval between payload repeats in milliseconds (default: 500)")
    parser.add_argument("--output", type=str, default=None,
                       help="Output filename (default: auto-generated)")

    args = parser.parse_args()

    # Set sample rate if not provided
    samp_rate = args.samp_rate if args.samp_rate else args.bw * 4
    
    # Generate output filename
    if args.output:
        output_file = args.output
    else:
        output_file = generate_filename(
            args.payload, args.sf, args.cr, args.bw, samp_rate,
            args.ldro_mode, not args.no_crc, args.impl_header
        )

    # Ensure vectors directory exists
    output_path = Path("vectors") / output_file
    output_path.parent.mkdir(parents=True, exist_ok=True)

    print(f"Generating LoRa vector with parameters:")
    print(f"  Payload: '{args.payload}'")
    print(f"  SF: {args.sf}, CR: {args.cr}, BW: {args.bw} Hz")
    print(f"  Sample Rate: {samp_rate} Hz")
    print(f"  LDRO: {args.ldro_mode}, CRC: {not args.no_crc}, Impl Header: {args.impl_header}")
    print(f"  Sync Word: 0x{args.sync_word:02x}")
    print(f"  Duration: {args.duration} seconds")
    print(f"  Strobe Period: {args.strobe_ms} ms")
    print(f"  Output: {output_path}")

    payload_message = pmt.intern(args.payload)

    tb = LoRaVectorGenerator(
        payload_pmt=payload_message,
        output_file=str(output_path),
        sf=args.sf,
        cr=args.cr,
        bw=args.bw,
        samp_rate=samp_rate,
        ldro_mode=args.ldro_mode,
        has_crc=not args.no_crc,
        impl_head=args.impl_header,
        sync_word=args.sync_word,
        strobe_ms=args.strobe_ms,
    )

    def sig_handler(sig=None, frame=None):
        try:
            tb.stop()
            tb.wait()
        finally:
            sys.exit(0)

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    tb.start()
    print("Generating vector... (Press Ctrl+C to stop)")
    
    try:
        time.sleep(args.duration)
    except KeyboardInterrupt:
        pass
    finally:
        tb.stop()
        tb.wait()
        print(f"Vector saved to: {output_path}")
        print(f"To decode: python3 external/gr_lora_sdr/scripts/decode_offline_recording.py {output_path}")


if __name__ == '__main__':
    main()

