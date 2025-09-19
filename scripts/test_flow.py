import numpy as np
from gnuradio import gr, blocks
import gnuradio.lora_sdr as lora_sdr
import pmt

data = np.fromfile('vectors/gnuradio_sf7_cr45_crc.bin', dtype=np.float32).view(np.complex64)

class Flow(gr.top_block):
    def __init__(self, data):
        gr.top_block.__init__(self)
        self.src = blocks.vector_source_c(data, False)
        self.sync = lora_sdr.frame_sync(0, 125000, 7, False, [0x34], 2, 8)
        self.fft = lora_sdr.fft_demod(False, True)
        self.gray = lora_sdr.gray_mapping(False)
        self.deinter = lora_sdr.deinterleaver(False)
        self.hamm = lora_sdr.hamming_dec(False)
        self.header = lora_sdr.header_decoder(False, 1, 115, True, 2, True)
        self.dewhite = lora_sdr.dewhitening()
        self.crc = lora_sdr.crc_verif(True, False)
        self.snk = blocks.vector_sink_b()
        self.msg = blocks.message_debug()
        self.msg_connect((self.header, 'frame_info'), (self.sync, 'frame_info'))
        self.msg_connect((self.header, 'frame_info'), (self.msg, 'store'))
        self.msg_connect((self.crc, 'msg'), (self.msg, 'store'))
        self.connect(self.src, self.sync, self.fft, self.gray, self.deinter, self.hamm,
                     self.header, self.dewhite, self.crc, self.snk)

tb = Flow(data)
tb.run()
print('payload bytes', len(tb.snk.data()))
print('messages', tb.msg.num_messages())
while tb.msg.num_messages() > 0:
    print(pmt.to_python(tb.msg.get_message(0)))
