#!/usr/bin/env python3
"""Run a GNU Radio LoRa receive chain on an IQ file.

This flowgraph stitches together the individual gr-lora_sdr blocks instead
of the hierarchical `lora_sdr_lora_rx` convenience wrapper.  The hierarchical
block has been unstable in our environment (segfaults when fed straight from
disk), but the primitive blocks appear to behave correctly when driven in
a simple top block.  That makes this script a practical substitute for
verifying reference vectors until the upstream issue is resolved.
"""

import argparse
import numpy as np
import pmt
from gnuradio import gr, blocks
import gnuradio.lora_sdr as lora_sdr


class LoRaRxFile(gr.top_block):
    def __init__(self, samples: np.ndarray, sf: int, cr: int, bw: int,
                 samp_rate: int, sync_word: int, payload_len: int,
                 has_crc: bool, implicit_header: bool, ldro_mode: int,
                 soft_decoding: bool, center_freq: float):
        gr.top_block.__init__(self, "lora_rx_file")

        # Hold a reference to the sample buffer so the underlying memory stays alive.
        self._buffer = np.asarray(samples, dtype=np.complex64)
        # Convert to a pure Python list to avoid lifetime issues inside vector_source_c.
        self.src = blocks.vector_source_c(self._buffer.tolist(), False)
        center_freq_i = int(center_freq)
        self.frame_sync = lora_sdr.frame_sync(center_freq_i, bw, sf,
                                             implicit_header, [sync_word],
                                             int(samp_rate / bw), 8)
        self.fft = lora_sdr.fft_demod(soft_decoding, True)
        self.gray = lora_sdr.gray_mapping(soft_decoding)
        self.deinter = lora_sdr.deinterleaver(soft_decoding)
        self.hamming = lora_sdr.hamming_dec(soft_decoding)
        self.header = lora_sdr.header_decoder(implicit_header, cr, payload_len,
                                              has_crc, ldro_mode, True)
        self.dewhite = lora_sdr.dewhitening()
        self.crc = lora_sdr.crc_verif(True, False)

        self.payload_sink = blocks.vector_sink_b()
        self.header_dbg = blocks.message_debug()
        self.crc_dbg = blocks.message_debug()

        # Connections – mirror the ordering in the hierarchical block.
        self.connect(self.src, self.frame_sync)
        self.connect(self.frame_sync, self.fft, self.gray, self.deinter,
                     self.hamming, self.header, self.dewhite, self.crc,
                     self.payload_sink)

        self.msg_connect((self.header, 'frame_info'), (self.header_dbg, 'store'))
        self.msg_connect((self.crc, 'msg'), (self.crc_dbg, 'store'))


def pop_messages(block: blocks.message_debug):
    out = []
    while block.num_messages() > 0:
        msg = block.get_message(0)
        out.append(pmt.to_python(msg))
    return out


def main():
    ap = argparse.ArgumentParser(description="Run GNU Radio LoRa RX on an IQ file")
    ap.add_argument('--iq', required=True, help='Path to interleaved float32 IQ file')
    ap.add_argument('--sf', type=int, required=True)
    ap.add_argument('--cr', type=int, required=True,
                    help='Coding rate index used by gr-lora_sdr (1=4/5 … 4=4/8)')
    ap.add_argument('--bw', type=int, default=125000)
    ap.add_argument('--samp-rate', type=int, default=250000)
    ap.add_argument('--sync', type=lambda x: int(x, 0), default=0x34)
    ap.add_argument('--payload-len', type=int, required=True)
    ap.add_argument('--has-crc', action='store_true')
    ap.add_argument('--implicit', action='store_true')
    ap.add_argument('--ldro', type=int, default=2)
    ap.add_argument('--soft-decoding', action='store_true')
    ap.add_argument('--center-freq', type=float, default=0.0,
                    help='Center frequency argument passed to frame_sync')
    ap.add_argument('--tail-samples', type=int, default=0,
                    help='Optional complex zeros appended to the stream for stability')
    args = ap.parse_args()

    raw = np.fromfile(args.iq, dtype=np.float32)
    if raw.size % 2:
        raise SystemExit('IQ file must contain an even number of float32 values')
    samples = raw.view(np.complex64)
    if args.tail_samples > 0:
        samples = np.concatenate([samples, np.zeros(args.tail_samples, dtype=np.complex64)])

    # Prepend a guard interval to avoid frame_sync accessing uninitialized memory
    guard = int(4 * (1 << args.sf))
    samples = np.concatenate([np.zeros(guard, dtype=np.complex64), samples])

    tb = LoRaRxFile(samples, args.sf, args.cr, args.bw, args.samp_rate,
                    args.sync, args.payload_len, args.has_crc,
                    args.implicit, args.ldro, args.soft_decoding,
                    args.center_freq)
    tb.run()

    payload_vec = tb.payload_sink.data()
    print(f'payload_len={len(payload_vec)}')


if __name__ == '__main__':
    main()
