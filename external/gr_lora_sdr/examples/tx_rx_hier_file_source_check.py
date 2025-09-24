#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#
# SPDX-License-Identifier: GPL-3.0
#
# Based on: tx_rx_hier_functionality_check.py
# Purpose: Send file contents as payload instead of "Hello world"

from gnuradio import blocks
import pmt
from gnuradio import channels
from gnuradio import gr
import sys
import signal
from argparse import ArgumentParser
import pathlib
import time
import gnuradio.lora_sdr as lora_sdr


class tx_rx_hier_file_source_check(gr.top_block):
    def __init__(self, payload_str: str, period_ms: int = 2000):
        gr.top_block.__init__(self, "Tx Rx Hier File Source Check", catch_exceptions=True)

        # Basic LoRa params (matching the original hierarchical example)
        self.bw = 125000
        self.sf = 7
        self.samp_rate = self.bw * 4
        self.center_freq = 868.1e6
        self.clk_offset = 0
        self.SNRdB = -5

        # Blocks
        self.lora_tx_0 = lora_sdr.lora_sdr_lora_tx(
            bw=self.bw,
            cr=2,
            has_crc=True,
            impl_head=False,
            samp_rate=self.samp_rate,
            sf=self.sf,
            ldro_mode=2,
            frame_zero_padd=int(20 * (2 ** self.sf) * self.samp_rate / self.bw),
            sync_word=[0x12],
        )
        self.lora_rx_0 = lora_sdr.lora_sdr_lora_rx(
            bw=self.bw,
            cr=2,
            has_crc=True,
            impl_head=False,
            pay_len=255,
            samp_rate=self.samp_rate,
            sf=self.sf,
            sync_word=[0x12],
            soft_decoding=True,
            ldro_mode=2,
            print_rx=[True, True],  # print header + payload
        )
        self.channels_channel_model_0 = channels.channel_model(
            noise_voltage=(10 ** (-self.SNRdB / 20.0)),
            frequency_offset=(self.center_freq * self.clk_offset * 1e-6 / self.samp_rate),
            epsilon=(1.0 + self.clk_offset * 1e-6),
            taps=[1.0 + 0.0j],
            noise_seed=0,
            block_tags=True,
        )
        # Ensure sufficient buffering at the channel output
        try:
            self.channels_channel_model_0.set_min_output_buffer(int((2 ** self.sf + 2) * self.samp_rate / self.bw))
        except Exception:
            pass
        self.blocks_throttle_0 = blocks.throttle(gr.sizeof_gr_complex * 1, int(self.samp_rate * 10), True)

        # Send the provided payload string periodically (like the original example)
        # Note: payload should be ASCII-safe for clean RX printing.
        self.blocks_message_strobe_0_0 = blocks.message_strobe(pmt.intern(payload_str), int(period_ms))

        # Connections
        # Message path: strobe -> TX
        self.msg_connect((self.blocks_message_strobe_0_0, 'strobe'), (self.lora_tx_0, 'in'))
        # Sample path: TX -> throttle -> channel -> RX
        self.connect((self.lora_tx_0, 0), (self.blocks_throttle_0, 0))
        self.connect((self.blocks_throttle_0, 0), (self.channels_channel_model_0, 0))
        self.connect((self.channels_channel_model_0, 0), (self.lora_rx_0, 0))


def _read_file_payload(path: pathlib.Path, max_bytes: int, encoding: str) -> str:
    # Try text read first to preserve readability at RX; fallback to bytes-as-latin1
    try:
        data = path.read_text(encoding=encoding, errors='ignore')
    except Exception:
        data = path.read_bytes().decode('latin-1', errors='ignore')
    if max_bytes is not None and max_bytes > 0:
        data = data[:max_bytes]
    # LoRa payload upper bound (without fragmentation) is 255 bytes
    if len(data) > 255:
        data = data[:255]
    return data


def main():
    ap = ArgumentParser(description='TX/RX check that transmits file contents as payload')
    ap.add_argument('--file', type=str, default=str(pathlib.Path('scripts/original_matrix.py')),
                    help='Path to a text file to transmit')
    ap.add_argument('--max-bytes', type=int, default=200,
                    help='Max bytes from file to send (<=255 recommended)')
    ap.add_argument('--encoding', type=str, default='utf-8', help='Text encoding for file read')
    ap.add_argument('--period-ms', type=int, default=2000, help='Repeat period for message strobe (ms)')
    args = ap.parse_args()

    path = pathlib.Path(args.file)
    if not path.exists():
        print(f"[error] file not found: {path}", file=sys.stderr)
        return 2

    payload = _read_file_payload(path, args.max_bytes, args.encoding)
    print(f"[info] sending {len(payload)} bytes from: {path}")

    tb = tx_rx_hier_file_source_check(payload, args.period_ms)

    def sig_handler(sig=None, frame=None):
        try:
            tb.stop()
            tb.wait()
        finally:
            sys.exit(0)

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    tb.start()
    try:
        input('Press Enter to quit... ')
    except EOFError:
        pass
    finally:
        tb.stop()
        tb.wait()


if __name__ == '__main__':
    raise SystemExit(main())

