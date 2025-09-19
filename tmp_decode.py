from gnuradio import gr, blocks
import gnuradio.lora_sdr as lora_sdr

IQ = 'vectors/gnuradio_sf7_cr47_crc.bin'

class Flow(gr.top_block):
    def __init__(self):
        gr.top_block.__init__(self, "decode", catch_exceptions=True)
        self.src = blocks.file_source(gr.sizeof_gr_complex, IQ, False)
        self.rx = lora_sdr.lora_sdr_lora_rx(bw=125000, cr=3, has_crc=True, impl_head=False, pay_len=255,
                                            samp_rate=250000, sf=7, sync_word=[0x34], soft_decoding=False,
                                            ldro_mode=2, print_rx=[True, True])
        self.msg = blocks.message_debug()
        self.snk = blocks.vector_sink_b()
        self.connect(self.src, self.rx, self.snk)
        self.msg_connect((self.rx, 'out'), (self.msg, 'store'))

def main():
    tb = Flow()
    tb.run()
    data = bytes(tb.snk.data())
    print('stream len', len(data))
    while tb.msg.num_messages() > 0:
        msg = tb.msg.get_message(0)
        print(msg)

if __name__ == '__main__':
    main()
