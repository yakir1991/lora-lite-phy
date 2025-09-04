#!/usr/bin/env python3
import argparse
import sys, os, time
from gnuradio import gr, blocks
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

        # Preprocess payload to match local model: CRC16(CCITT, init=0xFFFF, xorout=0),
        # then 8-bit LFSR whitening with poly x^8+x^6+x^5+x^4+1, seed 0xFF.
        def crc16_ccitt(data: bytes) -> int:
            poly = 0x1021
            crc = 0xFFFF
            for b in data:
                crc ^= (b << 8) & 0xFFFF
                for _ in range(8):
                    if crc & 0x8000:
                        crc = ((crc << 1) ^ poly) & 0xFFFF
                    else:
                        crc = (crc << 1) & 0xFFFF
            return crc & 0xFFFF
        def lfsr8_next(s: int) -> int:
            fb = ((s >> 7) ^ (s >> 5) ^ (s >> 4) ^ (s >> 3)) & 1
            return ((s << 1) & 0xFF) | fb
        crc = crc16_ccitt(payload_bytes)
        payload_crc = bytearray(payload_bytes)
        payload_crc.append((crc >> 8) & 0xFF)
        payload_crc.append(crc & 0xFF)
        s = 0xFF
        for i in range(len(payload_crc)):
            payload_crc[i] ^= s
            s = lfsr8_next(s)

        # Stream source of whitened bytes (payload+CRC) without header
        self.src = blocks.vector_source_b(list(payload_crc), False, 1)

        self.henc   = lora_sdr.hamming_enc(cr, sf)
        self.inter  = lora_sdr.interleaver(cr, sf, ldro, int(bw_hz))
        self.gray   = lora_sdr.gray_demap(sf)
        self.mod    = lora_sdr.modulate(sf, int(samp_rate_hz), int(bw_hz), sync_word, 0, preamb_len)

        self.sink = blocks.file_sink(gr.sizeof_gr_complex, out_iq_path, False)
        self.sink.set_unbuffered(True)

        # Stream wiring (no header, CRC already included and whitened)
        self.connect(self.src, self.henc)
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
    ap.add_argument('--timeout', type=float, default=15.0)
    args = ap.parse_args()

    with open(args.payload, 'rb') as f:
        data = f.read()
    print(f"[tx-pdu] sf={args.sf} cr={args.cr} len={len(data)} out={args.out}")

    tb = LoraTxPdu(args.sf, args.cr, data, args.out,
                   bw_hz=args.bw, samp_rate_hz=args.samp_rate,
                   preamb_len=args.preamble_len)
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
