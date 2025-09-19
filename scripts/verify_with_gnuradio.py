#!/usr/bin/env python3
import argparse
import numpy as np
from gnuradio import gr, blocks
import gnuradio.lora_sdr as lora_sdr

class VerifyFlow(gr.top_block):
    def __init__(self, samples, sf, cr, bw, samp_rate, sync_word, pay_len, has_crc, impl_head, ldro_mode):
        gr.top_block.__init__(self, "verify_vector", catch_exceptions=True)
        self.src = blocks.vector_source_c(samples.tolist(), False, 1, [])
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
            ldro_mode=ldro_mode,
            print_rx=[True, True],
        )
        self.snk = blocks.vector_sink_b()
        self.msg = blocks.message_debug()
        self.connect(self.src, self.rx, self.snk)
        self.msg_connect((self.rx, 'out'), (self.msg, 'store'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iq', required=True)
    ap.add_argument('--sf', type=int, required=True)
    ap.add_argument('--cr', type=int, required=True)
    ap.add_argument('--bw', type=int, default=125000)
    ap.add_argument('--samp-rate', type=int, default=250000)
    ap.add_argument('--sync', type=lambda x: int(x, 0), default=0x34)
    ap.add_argument('--payload-len', type=int, required=True)
    ap.add_argument('--has-crc', action='store_true')
    ap.add_argument('--implicit', action='store_true')
    ap.add_argument('--ldro', type=int, default=2)
    args = ap.parse_args()

    data = np.fromfile(args.iq, dtype=np.float32)
    if len(data) % 2 != 0:
        raise SystemExit('IQ file has odd number of float32 values')
    samples = data.view(np.complex64)

    tb = VerifyFlow(samples, args.sf, args.cr, args.bw, args.samp_rate,
                    args.sync, args.payload_len, args.has_crc,
                    args.implicit, args.ldro)
    tb.run()
    stream = bytes(tb.snk.data())
    print('stream_len', len(stream))
    if tb.msg.num_messages() == 0:
        print('no messages from RX')
    else:
        print('messages:', tb.msg.num_messages())
        import pmt
        while tb.msg.num_messages() > 0:
            msg = tb.msg.get_message(0)
            print('msg:', pmt.to_python(msg))
    print('stream_hex', stream.hex())

if __name__ == '__main__':
    main()
