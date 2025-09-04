#!/usr/bin/env python3
import argparse
import sys

from gnuradio import gr, blocks
import gnuradio.lora_sdr as lora_sdr


class LoraTxOnly(gr.top_block):
    def __init__(self, sf: int, cr_lora: int, payload_path: str, out_iq_path: str,
                 bw_hz: float = 125000.0, samp_rate_hz: float = 125000.0,
                 sync_word: int = 0x12, preamb_len: int = 8):
        gr.top_block.__init__(self, "lora_tx_only", catch_exceptions=True)

        # Convert LoRa CR (45..48) -> internal (1..4)
        if cr_lora not in (45, 46, 47, 48):
            raise ValueError("cr_lora must be one of 45,46,47,48")
        cr = cr_lora - 40

        # LDRO for sf>6 (matches LoRa spec)
        ldro = 1 if sf > 6 else 0

        # Blocks: bytes -> whitening -> add_crc -> hamming_enc -> interleaver ->
        #         gray_demap -> modulate -> file_sink (complex)
        self.src = blocks.file_source(gr.sizeof_char, payload_path, False)
        # Arguments: is_hex, use_length_tag, separator, length_tag_name
        self.whiten = lora_sdr.whitening(False, False)
        self.add_crc = lora_sdr.add_crc(True)
        self.hamming_enc = lora_sdr.hamming_enc(cr, sf)
        self.interleaver = lora_sdr.interleaver(cr, sf, ldro, int(bw_hz))
        self.gray_demap = lora_sdr.gray_demap(sf)
        self.mod = lora_sdr.modulate(sf, int(samp_rate_hz), int(bw_hz), [sync_word], 0, preamb_len)
        self.sink = blocks.file_sink(gr.sizeof_gr_complex, out_iq_path, False)
        self.sink.set_unbuffered(True)

        self.connect(self.src, self.whiten)
        self.connect(self.whiten, self.add_crc)
        self.connect(self.add_crc, self.hamming_enc)
        self.connect(self.hamming_enc, self.interleaver)
        self.connect(self.interleaver, self.gray_demap)
        self.connect(self.gray_demap, self.mod)
        self.connect(self.mod, self.sink)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--sf", type=int, required=True)
    ap.add_argument("--cr", type=int, required=True, help="LoRa coding rate as 45/46/47/48")
    ap.add_argument("--payload", required=True, help="Path to payload .bin file")
    ap.add_argument("--out", required=True, help="Output IQ file (.bin of float32 pairs)")
    ap.add_argument("--bw", type=float, default=125000.0)
    ap.add_argument("--samp-rate", type=float, default=125000.0)
    ap.add_argument("--preamble-len", type=int, default=8)
    args = ap.parse_args(argv)

    tb = LoraTxOnly(args.sf, args.cr, args.payload, args.out,
                    bw_hz=args.bw, samp_rate_hz=args.samp_rate, preamb_len=args.preamble_len)
    tb.start()
    tb.wait()
    return 0


if __name__ == "__main__":
    sys.exit(main())
