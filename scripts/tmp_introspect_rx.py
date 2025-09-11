from gnuradio.lora_sdr.lora_sdr_lora_rx import lora_sdr_lora_rx
rx = lora_sdr_lora_rx(center_freq=int(868.1e6), bw=125000, cr=1, has_crc=True, impl_head=False, pay_len=255, samp_rate=250000, sf=7, sync_word=[0x34], soft_decoding=False, ldro_mode=2, print_rx=[False, False])
attrs = [a for a in dir(rx) if not a.startswith('_')]
print('\n'.join(attrs))
