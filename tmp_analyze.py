import time
from gnuradio import gr, blocks
import pmt
import gnuradio.lora_sdr as lora_sdr

IQ='vectors/gnuradio_sf7_cr45_crc_ref.bin'
SF=7
CR=1
BW=125000
SAMP=250000
SYNC=[0x34]
LEN=22

class Flow(gr.top_block):
    def __init__(self):
        gr.top_block.__init__(self, "analyze")
        self.src = blocks.file_source(gr.sizeof_gr_complex, IQ, False)
        self.frame_sync = lora_sdr.frame_sync(868100000, BW, SF, False, SYNC, int(SAMP/BW), 8)
        self.fft = lora_sdr.fft_demod(False, True)
        self.gray = lora_sdr.gray_mapping(False)
        self.deinter = lora_sdr.deinterleaver(False)
        self.hamming = lora_sdr.hamming_dec(False)
        self.header = lora_sdr.header_decoder(False, CR, LEN, True, 2, True)
        self.dewhite = lora_sdr.dewhitening()
        self.crc = lora_sdr.crc_verif(True, False)
        self.snk = blocks.vector_sink_b()
        self.msg = blocks.message_debug()
        self.connect(self.src, self.frame_sync, self.fft, self.gray, self.deinter, self.hamming, self.header, self.dewhite, self.crc, self.snk)
        self.msg_connect((self.header, 'frame_info'), (self.msg, 'store'))

if __name__ == '__main__':
    tb = Flow()
    tb.start()
    time.sleep(3)
    tb.stop()
    tb.wait()
    data = tb.snk.data()
    print('decoded', len(data))
    print('first', data[:40])
    print('msgs', tb.msg.num_messages())
    while tb.msg.num_messages() > 0:
        m = tb.msg.get_message(0)
        print(pmt.to_python(m))
