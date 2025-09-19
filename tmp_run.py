import numpy as np
from gnuradio import gr, blocks
import gnuradio.lora_sdr as l
import pmt

data = np.fromfile('vectors/gnuradio_sf7_cr45_crc.bin', dtype=np.float32).view(np.complex64)

class Flow(gr.top_block):
    def __init__(self):
        gr.top_block.__init__(self)
        self.src = blocks.vector_source_c(data.tolist(), False)
        self.sync = l.frame_sync(0,125000,7,False,[0x34],2,8)
        self.fft = l.fft_demod(False, True)
        self.gray = l.gray_mapping(False)
        self.deinter = l.deinterleaver(False)
        self.hamm = l.hamming_dec(False)
        self.header = l.header_decoder(False, 1, 115, True, 2, True)
        self.dewhite = l.dewhitening()
        self.crc = l.crc_verif(True, False)
        self.snk = blocks.vector_sink_b()
        self.msg = blocks.message_debug()
        self.msg_connect((self.header,'frame_info'),(self.msg,'store'))
        self.msg_connect((self.crc,'msg'),(self.msg,'store'))
        self.connect(self.src,self.sync,self.fft,self.gray,self.deinter,
                     self.hamm,self.header,self.dewhite,self.crc,self.snk)

f=Flow()
f.run()
print('len', len(f.snk.data()))
print('msgs', f.msg.num_messages())
while f.msg.num_messages():
    print(pmt.to_python(f.msg.get_message(0)))
