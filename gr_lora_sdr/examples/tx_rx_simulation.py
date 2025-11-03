#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#
# SPDX-License-Identifier: GPL-3.0
#
# GNU Radio Python Flow Graph
# Title: Tx Rx Simulation
# Author: Tapparel Joachim@EPFL,TCL
# GNU Radio version: 3.10.11.0

from gnuradio import blocks
import pmt
from gnuradio import channels
from gnuradio.filter import firdes
from gnuradio import gr
from gnuradio.fft import window
import sys
import signal
from argparse import ArgumentParser
from gnuradio.eng_arg import eng_float, intx
from gnuradio import eng_notation
import gnuradio.lora_sdr as lora_sdr
import threading
import numpy as np
import os
import time
from pathlib import Path




class tx_rx_simulation(gr.top_block):

    def __init__(self):
        gr.top_block.__init__(self, "Tx Rx Simulation", catch_exceptions=True)
        self.flowgraph_started = threading.Event()

        ##################################################
        # Variables
        ##################################################
        self.soft_decoding = soft_decoding = False
        self.sf = sf = 7
        self.samp_rate = samp_rate = 500000
        self.preamb_len = preamb_len = 8
        self.pay_len = pay_len = 16
        self.ldro = ldro = False
        self.impl_head = impl_head = False
        self.has_crc = has_crc = True
        self.cr = cr = 2
        self.clk_offset = clk_offset = 0.0
        self.center_freq = center_freq = 868.1e6
        self.sync_word = sync_word = 0x12
        self.bw = bw = 125000
        self.SNRdB = SNRdB = -5.0

        def _env_bool(name: str, default: bool) -> bool:
            value = os.getenv(name)
            if value is None:
                return default
            return value not in {"0", "false", "False"}

        self.soft_decoding = soft_decoding = _env_bool("LORA_SOFT_DECODING", soft_decoding)
        self.sf = sf = int(os.getenv("LORA_SF", sf))
        self.samp_rate = samp_rate = int(os.getenv("LORA_SAMPLE_RATE", samp_rate))
        self.preamb_len = preamb_len = int(os.getenv("LORA_PREAMBLE_LEN", preamb_len))
        self.pay_len = pay_len = int(os.getenv("LORA_PAYLOAD_LEN", pay_len))
        self.ldro = ldro = _env_bool("LORA_LDRO", ldro)
        self.impl_head = impl_head = _env_bool("LORA_IMPLICIT_HEADER", impl_head)
        self.has_crc = has_crc = _env_bool("LORA_HAS_CRC", has_crc)
        self.cr = cr = int(os.getenv("LORA_CR", cr))
        self.clk_offset = clk_offset = float(os.getenv("LORA_CLK_OFFSET_PPM", clk_offset))
        self.center_freq = center_freq = float(os.getenv("LORA_CENTER_FREQ", center_freq))
        self.sync_word = sync_word = int(os.getenv("LORA_SYNC_WORD", sync_word))
        self.bw = bw = int(os.getenv("LORA_BW", bw))
        self.SNRdB = SNRdB = float(os.getenv("LORA_SNR_DB", SNRdB))

        ##################################################
        # Blocks
        ##################################################

        self.lora_sdr_whitening_0 = lora_sdr.whitening(False,True,',','packet_len')
        self.lora_sdr_modulate_0 = lora_sdr.modulate(sf, samp_rate, bw, [sync_word], (int(20*2**sf*samp_rate/bw)),preamb_len)
        self.lora_sdr_interleaver_0 = lora_sdr.interleaver(cr, sf, ldro, bw)
        self.lora_sdr_header_decoder_0 = lora_sdr.header_decoder(impl_head, cr, pay_len, has_crc, ldro, True)
        self.lora_sdr_header_0 = lora_sdr.header(impl_head, has_crc, cr)
        self.lora_sdr_hamming_enc_0 = lora_sdr.hamming_enc(cr, sf)
        self.lora_sdr_hamming_dec_0 = lora_sdr.hamming_dec(soft_decoding)
        self.lora_sdr_gray_mapping_0 = lora_sdr.gray_mapping( soft_decoding)
        self.lora_sdr_gray_demap_0 = lora_sdr.gray_demap(sf)
        self.lora_sdr_frame_sync_0 = lora_sdr.frame_sync(int(center_freq), bw, sf, impl_head, [sync_word], (int(samp_rate/bw)),preamb_len)
        self.lora_sdr_fft_demod_0 = lora_sdr.fft_demod( soft_decoding, False)
        self.lora_sdr_dewhitening_0 = lora_sdr.dewhitening()
        self.lora_sdr_deinterleaver_0 = lora_sdr.deinterleaver( soft_decoding)
        self.lora_sdr_crc_verif_0 = lora_sdr.crc_verif( 1, False)
        self.lora_sdr_add_crc_0 = lora_sdr.add_crc(has_crc)
        self.channels_channel_model_0 = channels.channel_model(
            noise_voltage=(10**(-SNRdB/20)),
            frequency_offset=(center_freq*clk_offset*1e-6/samp_rate),
            epsilon=(1.0+clk_offset*1e-6),
            taps=[1.0 + 0.0j],
            noise_seed=0,
            block_tags=True)
        self.channels_channel_model_0.set_min_output_buffer((int(2**sf*samp_rate/bw*1.1)))
        self.blocks_throttle_0 = blocks.throttle(gr.sizeof_gr_complex*1, (samp_rate*10),True)
        self._repo_root = Path(__file__).resolve().parents[1]
        payload_override = os.getenv("LORA_PAYLOAD_SOURCE")
        if payload_override:
            self._example_source_path = payload_override
        else:
            self._example_source_path = str(
                self._repo_root / "data" / "GRC_default" / "example_tx_source.txt"
            )

        output_root = os.getenv("LORA_OUTPUT_DIR")
        if output_root:
            self._generated_dir = Path(output_root)
        else:
            self._generated_dir = self._repo_root / "data" / "generated"
        self._generated_dir.mkdir(parents=True, exist_ok=True)

        if not Path(self._example_source_path).exists():
            raise FileNotFoundError(f"Payload source not found: {self._example_source_path}")

        self._dump_debug = _env_bool("LORA_DUMP_DEBUG", False)

        self.blocks_file_source_0_0 = blocks.file_source(gr.sizeof_char*1, self._example_source_path, True, 0, 0)
        self.blocks_file_source_0_0.set_begin_tag(pmt.PMT_NIL)
        self.blocks_stream_to_tagged_stream_0 = blocks.stream_to_tagged_stream(gr.sizeof_char, 1, pay_len, "packet_len")
        self.blocks_vector_sink_0 = blocks.vector_sink_c()
        if self._dump_debug:
            self.blocks_vector_sink_gray = blocks.vector_sink_s()
            self.blocks_vector_sink_deint = blocks.vector_sink_b()
            self.blocks_vector_sink_hamming = blocks.vector_sink_b()
            self.blocks_vector_sink_fft = blocks.vector_sink_s()
        else:
            self.blocks_vector_sink_gray = None
            self.blocks_vector_sink_deint = None
            self.blocks_vector_sink_hamming = None
            self.blocks_vector_sink_fft = None

        output_name_env = os.getenv("LORA_OUTPUT_NAME")
        if output_name_env:
            output_name = output_name_env
        else:
            snr_token = f"{'p' if SNRdB >= 0 else 'm'}{abs(SNRdB):.1f}".replace('.', 'p')
            output_name = f"tx_rx_sf{sf}_bw{bw}_cr{cr}_snr{snr_token}.cf32"
        self._output_path = self._generated_dir / output_name


        ##################################################
        # Connections
        ##################################################
        self.msg_connect((self.lora_sdr_header_decoder_0, 'frame_info'), (self.lora_sdr_frame_sync_0, 'frame_info'))
        self.connect((self.blocks_file_source_0_0, 0), (self.blocks_stream_to_tagged_stream_0, 0))
        self.connect((self.blocks_stream_to_tagged_stream_0, 0), (self.lora_sdr_whitening_0, 0))
        self.connect((self.lora_sdr_whitening_0, 0), (self.lora_sdr_header_0, 0))
        self.connect((self.blocks_throttle_0, 0), (self.channels_channel_model_0, 0))
        self.connect((self.channels_channel_model_0, 0), (self.lora_sdr_frame_sync_0, 0))
        self.connect((self.channels_channel_model_0, 0), (self.blocks_vector_sink_0, 0))
        self.connect((self.lora_sdr_add_crc_0, 0), (self.lora_sdr_hamming_enc_0, 0))
        self.connect((self.lora_sdr_deinterleaver_0, 0), (self.lora_sdr_hamming_dec_0, 0))
        self.connect((self.lora_sdr_dewhitening_0, 0), (self.lora_sdr_crc_verif_0, 0))
        self.connect((self.lora_sdr_fft_demod_0, 0), (self.lora_sdr_gray_mapping_0, 0))
        self.connect((self.lora_sdr_frame_sync_0, 0), (self.lora_sdr_fft_demod_0, 0))
        self.connect((self.lora_sdr_gray_demap_0, 0), (self.lora_sdr_modulate_0, 0))
        self.connect((self.lora_sdr_gray_mapping_0, 0), (self.lora_sdr_deinterleaver_0, 0))
        self.connect((self.lora_sdr_hamming_dec_0, 0), (self.lora_sdr_header_decoder_0, 0))
        self.connect((self.lora_sdr_hamming_enc_0, 0), (self.lora_sdr_interleaver_0, 0))
        self.connect((self.lora_sdr_header_0, 0), (self.lora_sdr_add_crc_0, 0))
        self.connect((self.lora_sdr_header_decoder_0, 0), (self.lora_sdr_dewhitening_0, 0))
        self.connect((self.lora_sdr_interleaver_0, 0), (self.lora_sdr_gray_demap_0, 0))
        self.connect((self.lora_sdr_modulate_0, 0), (self.blocks_throttle_0, 0))
        if self._dump_debug:
            self.connect((self.lora_sdr_gray_mapping_0, 0), (self.blocks_vector_sink_gray, 0))
            self.connect((self.lora_sdr_deinterleaver_0, 0), (self.blocks_vector_sink_deint, 0))
            self.connect((self.lora_sdr_hamming_dec_0, 0), (self.blocks_vector_sink_hamming, 0))
            self.connect((self.lora_sdr_fft_demod_0, 0), (self.blocks_vector_sink_fft, 0))


    def get_soft_decoding(self):
        return self.soft_decoding

    def set_soft_decoding(self, soft_decoding):
        self.soft_decoding = soft_decoding

    def get_sf(self):
        return self.sf

    def set_sf(self, sf):
        self.sf = sf
        self.lora_sdr_gray_demap_0.set_sf(self.sf)
        self.lora_sdr_hamming_enc_0.set_sf(self.sf)
        self.lora_sdr_interleaver_0.set_sf(self.sf)

    def get_samp_rate(self):
        return self.samp_rate

    def set_samp_rate(self, samp_rate):
        self.samp_rate = samp_rate
        self.blocks_throttle_0.set_sample_rate((self.samp_rate*10))
        self.channels_channel_model_0.set_frequency_offset((self.center_freq*self.clk_offset*1e-6/self.samp_rate))

    def get_preamb_len(self):
        return self.preamb_len

    def set_preamb_len(self, preamb_len):
        self.preamb_len = preamb_len

    def get_pay_len(self):
        return self.pay_len

    def set_pay_len(self, pay_len):
        self.pay_len = pay_len

    def get_ldro(self):
        return self.ldro

    def set_ldro(self, ldro):
        self.ldro = ldro

    def get_impl_head(self):
        return self.impl_head

    def set_impl_head(self, impl_head):
        self.impl_head = impl_head

    def get_has_crc(self):
        return self.has_crc

    def set_has_crc(self, has_crc):
        self.has_crc = has_crc

    def get_cr(self):
        return self.cr

    def set_cr(self, cr):
        self.cr = cr
        self.lora_sdr_hamming_enc_0.set_cr(self.cr)
        self.lora_sdr_header_0.set_cr(self.cr)
        self.lora_sdr_interleaver_0.set_cr(self.cr)

    def get_clk_offset(self):
        return self.clk_offset

    def set_clk_offset(self, clk_offset):
        self.clk_offset = clk_offset
        self.channels_channel_model_0.set_frequency_offset((self.center_freq*self.clk_offset*1e-6/self.samp_rate))
        self.channels_channel_model_0.set_timing_offset((1.0+self.clk_offset*1e-6))

    def get_center_freq(self):
        return self.center_freq

    def set_center_freq(self, center_freq):
        self.center_freq = center_freq
        self.channels_channel_model_0.set_frequency_offset((self.center_freq*self.clk_offset*1e-6/self.samp_rate))

    def get_bw(self):
        return self.bw

    def set_bw(self, bw):
        self.bw = bw

    def get_SNRdB(self):
        return self.SNRdB

    def set_SNRdB(self, SNRdB):
        self.SNRdB = SNRdB
        self.channels_channel_model_0.set_noise_voltage((10**(-self.SNRdB/20)))

    def dump_generated_iq(self):
        data = np.array(self.blocks_vector_sink_0.data(), dtype=np.complex64)
        self._output_path.parent.mkdir(parents=True, exist_ok=True)
        if data.size == 0:
            return self._output_path, 0
        data.tofile(self._output_path)
        return self._output_path, data.size

    def dump_debug_vectors(self):
        if not self._dump_debug:
            return []
        dump_paths = []
        stem = self._output_path.stem
        if self.blocks_vector_sink_gray is not None:
            gray = np.array(self.blocks_vector_sink_gray.data(), dtype=np.int16)
            path = self._generated_dir / f"{stem}_gray.txt"
            np.savetxt(path, gray, fmt="%d")
            dump_paths.append(path)
        if self.blocks_vector_sink_deint is not None:
            deint = np.array(self.blocks_vector_sink_deint.data(), dtype=np.uint8)
            path = self._generated_dir / f"{stem}_deinterleaver.txt"
            np.savetxt(path, deint, fmt="%d")
            dump_paths.append(path)
        if self.blocks_vector_sink_hamming is not None:
            ham = np.array(self.blocks_vector_sink_hamming.data(), dtype=np.uint8)
            path = self._generated_dir / f"{stem}_hamming.txt"
            np.savetxt(path, ham, fmt="%d")
            dump_paths.append(path)
        if self.blocks_vector_sink_fft is not None:
            fft_vals = np.array(self.blocks_vector_sink_fft.data(), dtype=np.int16)
            path = self._generated_dir / f"{stem}_fft.txt"
            np.savetxt(path, fft_vals, fmt="%d")
            dump_paths.append(path)
        return dump_paths



def main(top_block_cls=tx_rx_simulation, options=None):
    tb = top_block_cls()

    def sig_handler(sig=None, frame=None):
        tb.stop()
        tb.wait()

        sys.exit(0)

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    autostop = os.environ.get("LORA_AUTOSTOP_SECS")
    if autostop is not None:
        try:
            duration = float(autostop)
        except ValueError:
            duration = 5.0
        tb.start()
        tb.flowgraph_started.set()
        time.sleep(max(duration, 0.0))
        tb.stop()
        tb.wait()
        out_path, written = tb.dump_generated_iq()
        print(f"[tx_rx_simulation] wrote {written} complex samples to {out_path}")
        debug_paths = tb.dump_debug_vectors()
        for debug_path in debug_paths:
            print(f"[tx_rx_simulation] dumped debug data to {debug_path}")
        return

    tb.start()
    tb.flowgraph_started.set()

    try:
        input('Press Enter to quit: ')
    except EOFError:
        pass
    tb.stop()
    tb.wait()
    out_path, written = tb.dump_generated_iq()
    print(f"[tx_rx_simulation] wrote {written} complex samples to {out_path}")
    debug_paths = tb.dump_debug_vectors()
    for debug_path in debug_paths:
        print(f"[tx_rx_simulation] dumped debug data to {debug_path}")


if __name__ == '__main__':
    main()
