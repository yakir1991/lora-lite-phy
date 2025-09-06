#!/usr/bin/env python3
import argparse
import sys, os, time
from gnuradio import gr, blocks
import pmt
import gnuradio.lora_sdr as lora_sdr


class LoraTxPdu(gr.top_block):
    def __init__(self, sf: int, cr_lora: int, payload_bytes: bytes, out_iq_path: str,
                 bw_hz: int = 125000, samp_rate_hz: int = 125000,
                 sync_word = [0x12], preamb_len: int = 0):
        gr.top_block.__init__(self, "lora_tx_pdu", catch_exceptions=True)

        if cr_lora not in (45, 46, 47, 48):
            raise ValueError("cr_lora must be 45/46/47/48")
        cr = cr_lora - 40
        ldro = 1 if sf > 6 else 0

        # Build a message path: hex PDU -> whitening (is_hex, use_length_tag) -> header -> add_crc
        # -> hamming_enc -> interleaver -> gray_demap -> modulate -> file_sink
        hex_msg = ','.join(f"{b:02x}" for b in payload_bytes)
        self.msg_src = blocks.message_strobe(pmt.intern(hex_msg), 1000)
        self.whiten  = lora_sdr.whitening(True, True, ',', 'packet_len')
        self.header  = lora_sdr.header(False, True, cr)
        self.add_crc = lora_sdr.add_crc(True)
        self.henc    = lora_sdr.hamming_enc(cr, sf)
        self.inter   = lora_sdr.interleaver(cr, sf, ldro, int(bw_hz))
        self.gray    = lora_sdr.gray_demap(sf)
        # Compute frame zero padding similar to example (to satisfy output multiple)
        frame_zero_padd = int(20 * (2**sf) * samp_rate_hz / bw_hz)
        # Allow custom sync word list (e.g., [0x34] for public network)
        self.mod     = lora_sdr.modulate(sf, int(samp_rate_hz), int(bw_hz), sync_word, frame_zero_padd, preamb_len)

        self.sink = blocks.file_sink(gr.sizeof_gr_complex, out_iq_path, False)
        self.sink.set_unbuffered(True)

        # Messages
        self.msg_connect(self.msg_src, 'strobe', self.whiten, 'msg')
        # Stream wiring
        self.connect(self.whiten, self.header)
        self.connect(self.header, self.add_crc)
        self.connect(self.add_crc, self.henc)
        self.connect(self.henc, self.inter)
        self.connect(self.inter, self.gray)
        self.connect(self.gray, self.mod)
        self.connect(self.mod, self.sink)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sf', type=int, required=True)
    ap.add_argument('--cr', type=int, required=True)
    ap.add_argument('--payload', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--bw', type=int, default=125000)
    ap.add_argument('--samp-rate', type=int, default=125000)
    ap.add_argument('--preamble-len', type=int, default=0)
    ap.add_argument('--sync', default='0x12', help='sync word byte (e.g., 0x12 or 0x34)')
    ap.add_argument('--timeout', type=float, default=15.0)
    args = ap.parse_args()

    with open(args.payload, 'rb') as f:
        data = f.read()
    print(f"[tx-pdu] sf={args.sf} cr={args.cr} len={len(data)} out={args.out}")

    try:
        sync_val = int(args.sync, 0)
    except Exception:
        sync_val = 0x12
    tb = LoraTxPdu(args.sf, args.cr, data, args.out,
                   bw_hz=args.bw, samp_rate_hz=args.samp_rate,
                   sync_word=[sync_val], preamb_len=args.preamble_len)
    try:
        tb.start()
        start = time.time(); last = -1
        while True:
            sz = os.path.getsize(args.out) if os.path.exists(args.out) else 0
            if sz != last:
                print(f"[tx-pdu] out size={sz}")
                last = sz
            if sz > 0:
                break
            if time.time() - start > args.timeout:
                print("[tx-pdu] timeout")
                return 2
            time.sleep(0.1)
        time.sleep(0.25)
    finally:
        tb.stop(); tb.wait()
    return 0


if __name__ == '__main__':
    sys.exit(main())
