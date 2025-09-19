#!/usr/bin/env python3
import argparse
from gnuradio import gr, blocks
import pmt
from gnuradio import lora_sdr

class GnuradioLoRaRunner(gr.top_block):
    def __init__(self, iq_path, sf, cr, bw, samp_rate, has_crc, impl_head, pay_len, sync_word):
        gr.top_block.__init__(self, "lora_gnuradio_runner")
        self.src = blocks.file_source(gr.sizeof_gr_complex, iq_path, False)
        self.rx = lora_sdr.lora_sdr_lora_rx(
            center_freq=0,
            bw=bw,
            cr=cr,
            has_crc=has_crc,
            impl_head=impl_head,
            pay_len=pay_len,
            samp_rate=samp_rate,
            sf=sf,
            sync_word=[sync_word],
            soft_decoding=False,
            ldro_mode=0,
            print_rx=[True, True],
        )
        self.stream_sink = blocks.vector_sink_b()
        self.msg_debug = blocks.message_debug()

        self.connect(self.src, self.rx, self.stream_sink)
        self.msg_connect((self.rx, 'out'), (self.msg_debug, 'store'))

    def run_and_collect(self):
        self.run()
        data = bytearray(self.stream_sink.data())
        print("stream_bytes", len(data))
        print("messages", self.msg_debug.num_messages())
        while self.msg_debug.num_messages() > 0:
            msg = self.msg_debug.get_message(0)
            if pmt.is_pair(msg):
                vector = pmt.cdr(msg)
                if pmt.is_u8vector(vector):
                    data.extend(pmt.u8vector_elements(vector))
        return bytes(data)


def main():
    parser = argparse.ArgumentParser(description="Run GNU Radio LoRa SDR pipeline on an IQ capture")
    parser.add_argument("--iq", required=True, help="Path to interleaved float32 IQ file")
    parser.add_argument("--sf", type=int, required=True)
    parser.add_argument("--cr", type=int, required=True, help="Coding rate index (1=4/5,2=4/6,...) as used by gr-lora")
    parser.add_argument("--bw", type=int, default=125000)
    parser.add_argument("--samp-rate", type=int, default=250000)
    parser.add_argument("--sync", type=lambda x: int(x, 0), default=0x12)
    parser.add_argument("--payload-len", type=int, required=True)
    parser.add_argument("--has-crc", action="store_true")
    parser.add_argument("--implicit", action="store_true", help="Use implicit header mode")
    parser.add_argument("--out", help="Output file to store decoded payload bytes")
    args = parser.parse_args()

    runner = GnuradioLoRaRunner(
        iq_path=args.iq,
        sf=args.sf,
        cr=args.cr,
        bw=args.bw,
        samp_rate=args.samp_rate,
        has_crc=args.has_crc,
        impl_head=args.implicit,
        pay_len=args.payload_len,
        sync_word=args.sync,
    )
    payload = runner.run_and_collect()

    if args.out:
        with open(args.out, "wb") as f:
            f.write(payload)

    print("decoded_bytes:", payload.hex())
    print("total_bytes:", len(payload))


if __name__ == "__main__":
    main()
