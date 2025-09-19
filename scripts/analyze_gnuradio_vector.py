#!/usr/bin/env python3
import argparse
import pmt
from gnuradio import gr, blocks, lora_sdr


class LoRaAnalyzer(gr.top_block):
    def __init__(self, iq_path, sf, cr, bw, samp_rate, has_crc, impl_head, pay_len, sync_word, ldro_mode):
        gr.top_block.__init__(self, "lora_gnuradio_analyzer")

        self.src = blocks.file_source(gr.sizeof_gr_complex, iq_path, False)
        self.src_probe = blocks.vector_sink_c()
        self.frame_sync = lora_sdr.frame_sync(868100000, bw, sf, impl_head, [sync_word], int(samp_rate / bw), 8)
        self.fft = lora_sdr.fft_demod(False, True)
        self.gray = lora_sdr.gray_mapping(False)
        self.deinter = lora_sdr.deinterleaver(False)
        self.hamming = lora_sdr.hamming_dec(False)
        self.header = lora_sdr.header_decoder(impl_head, cr, pay_len, has_crc, ldro_mode, True)
        self.dewhiten = lora_sdr.dewhitening()
        self.crc = lora_sdr.crc_verif(True, False)

        self.fft_sink = blocks.vector_sink_s()
        self.gray_sink = blocks.vector_sink_s()
        self.header_sink = blocks.vector_sink_b()
        self.dewhite_sink = blocks.vector_sink_b()
        self.payload_sink = blocks.vector_sink_b()

        self.header_msg = blocks.message_debug()
        self.crc_msg = blocks.message_debug()

        self.connect(self.src, self.frame_sync)
        self.connect(self.src, self.src_probe)
        self.connect(self.frame_sync, self.fft)
        self.connect(self.fft, self.gray)
        self.connect(self.gray, self.deinter)
        self.connect(self.deinter, self.hamming)
        self.connect(self.hamming, self.header)
        self.connect(self.header, self.dewhiten)
        self.connect(self.dewhiten, self.crc)

        self.connect(self.fft, self.fft_sink)
        self.connect(self.gray, self.gray_sink)
        self.connect(self.header, self.header_sink)
        self.connect(self.dewhiten, self.dewhite_sink)
        self.connect(self.crc, self.payload_sink)

        self.msg_connect((self.header, 'frame_info'), (self.header_msg, 'store'))
        self.msg_connect((self.header, 'frame_info'), (self.frame_sync, 'frame_info'))
        self.msg_connect((self.crc, 'msg'), (self.crc_msg, 'store'))

    def collect(self, run_seconds: float = 1.0):
        _ = run_seconds
        self.run()

        def pop_messages(debug_block):
            out = []
            while debug_block.num_messages() > 0:
                msg = debug_block.get_message(0)
                out.append(pmt.to_python(msg))
            return out

        return {
            'src_samples': list(self.src_probe.data()),
            'fft_bins': list(self.fft_sink.data()),
            'gray_symbols': list(self.gray_sink.data()),
            'header_stream': list(self.header_sink.data()),
            'dewhiten_stream': list(self.dewhite_sink.data()),
            'payload_stream': list(self.payload_sink.data()),
            'header_messages': pop_messages(self.header_msg),
            'crc_messages': pop_messages(self.crc_msg),
        }


def main():
    ap = argparse.ArgumentParser(description="Analyze IQ capture via GNURadio LoRa blocks")
    ap.add_argument('--iq', required=True)
    ap.add_argument('--sf', type=int, required=True)
    ap.add_argument('--cr', type=int, required=True)
    ap.add_argument('--bw', type=int, default=125000)
    ap.add_argument('--samp-rate', type=int, default=250000)
    ap.add_argument('--sync', type=lambda x: int(x, 0), default=0x12)
    ap.add_argument('--payload-len', type=int, required=True)
    ap.add_argument('--has-crc', action='store_true')
    ap.add_argument('--implicit', action='store_true')
    ap.add_argument('--ldro', type=int, default=2)
    ap.add_argument('--runtime', type=float, default=1.0, help='Runtime in seconds for flowgraph execution')
    args = ap.parse_args()

    tb = LoRaAnalyzer(
        iq_path=args.iq,
        sf=args.sf,
        cr=args.cr,
        bw=args.bw,
        samp_rate=args.samp_rate,
        has_crc=args.has_crc,
        impl_head=args.implicit,
        pay_len=args.payload_len,
        sync_word=args.sync,
        ldro_mode=args.ldro,
    )

    result = tb.collect(run_seconds=args.runtime)

    def preview(label, data, count=16):
        print(f"{label} (len={len(data)}):", list(data[:count]))
    preview("src_samples", result['src_samples'], 8)
    preview("fft_bins", result['fft_bins'])
    preview("gray_symbols", result['gray_symbols'])
    preview("header_stream", result['header_stream'], 32)
    preview("dewhite_stream", result['dewhiten_stream'], 32)
    preview("payload_stream", result['payload_stream'], 32)

    print("header_messages:")
    for msg in result['header_messages']:
        print(msg)

    print("crc_messages:")
    for msg in result['crc_messages']:
        print(msg)


if __name__ == '__main__':
    main()
