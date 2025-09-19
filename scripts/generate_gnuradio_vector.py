#!/usr/bin/env python3
import argparse
import time
import numpy as np
from gnuradio import gr, blocks
import pmt
import gnuradio.lora_sdr as lora_sdr


def build_payload_bytes(payload_spec: str, encoding: str) -> bytes:
    if payload_spec.startswith('hex:'):
        hex_str = payload_spec[4:].replace(' ', '')
        return bytes.fromhex(hex_str)
    if payload_spec.startswith('text:'):
        return payload_spec[5:].encode(encoding)
    return payload_spec.encode(encoding)


class LoRaVectorGenerator(gr.top_block):
    def __init__(self, payload: bytes, sf: int, cr: int, bw: int, samp_rate: int,
                 sync_word: int, has_crc: bool, implicit_header: bool, ldro_mode: int,
                 frame_zero_padd: int):
        gr.top_block.__init__(self, "LoRa GNU Radio Vector Generator", catch_exceptions=True)

        message_text = payload.decode('latin-1', errors='ignore')
        msg = pmt.intern(message_text)
        self.strobe = blocks.message_strobe(msg, 1000)

        self.tx = lora_sdr.lora_sdr_lora_tx(
            bw=bw,
            cr=cr,
            has_crc=has_crc,
            impl_head=implicit_header,
            samp_rate=samp_rate,
            sf=sf,
            ldro_mode=ldro_mode,
            frame_zero_padd=frame_zero_padd,
            sync_word=[sync_word],
        )

        self.sink = blocks.vector_sink_c()

        self.msg_connect((self.strobe, 'strobe'), (self.tx, 'in'))
        self.connect((self.tx, 0), (self.sink, 0))


    def generate(self, run_seconds: float) -> np.ndarray:
        self.start()
        try:
            time.sleep(run_seconds)
        finally:
            self.stop()
            self.wait()
        data = np.array(self.sink.data(), dtype=np.complex64)
        return data


def main():
    ap = argparse.ArgumentParser(description="Generate LoRa IQ vector using gr-lora_sdr")
    ap.add_argument('--payload', type=str, default='text:GNU Radio LoRa test payload',
                    help="Payload specification. Use 'text:...' or 'hex:...' format")
    ap.add_argument('--encoding', type=str, default='utf-8')
    ap.add_argument('--sf', type=int, default=7)
    ap.add_argument('--cr', type=int, default=1, help='Coding rate index (1=4/5 … 4=4/8)')
    ap.add_argument('--bw', type=int, default=125000)
    ap.add_argument('--samp-rate', type=int, default=250000)
    ap.add_argument('--sync', type=lambda x: int(x, 0), default=0x34)
    ap.add_argument('--has-crc', action='store_true', default=True)
    ap.add_argument('--implicit-header', action='store_true')
    ap.add_argument('--ldro-mode', type=int, default=2)
    ap.add_argument('--frame-guard', type=int, default=None,
                    help='Optional frame_zero_padd override (defaults to 20*2^sf*samp_rate/bw)')
    ap.add_argument('--run-seconds', type=float, default=0.5)
    ap.add_argument('--out', type=str, default='vectors/gnuradio_sf7_cr45_payload.bin')
    args = ap.parse_args()

    payload = build_payload_bytes(args.payload, args.encoding)
    if len(payload) == 0:
        raise SystemExit("Payload is empty")

    frame_zero_padd = args.frame_guard
    if frame_zero_padd is None:
        frame_zero_padd = int(20 * (2 ** args.sf) * args.samp_rate / args.bw)

    tb = LoRaVectorGenerator(
        payload=payload,
        sf=args.sf,
        cr=args.cr,
        bw=args.bw,
        samp_rate=args.samp_rate,
        sync_word=args.sync,
        has_crc=args.has_crc,
        implicit_header=args.implicit_header,
        ldro_mode=args.ldro_mode,
        frame_zero_padd=frame_zero_padd,
    )

    iq = tb.generate(args.run_seconds)
    print(f"Generated samples: {len(iq)}")

    iq_float = iq.view(np.float32)
    out_path = args.out
    iq_float.tofile(out_path)
    print(f"IQ vector written to {out_path}")
    print(f"Payload bytes ({len(payload)}): {payload.hex()}")


if __name__ == '__main__':
    main()
