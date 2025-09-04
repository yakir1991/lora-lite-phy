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

        # Blocks: bytes -> whitening -> header -> add_crc -> hamming_enc ->
        #         interleaver -> gray_demap -> modulate -> file_sink (complex)
        self.src = blocks.file_source(gr.sizeof_char, payload_path, False)
        # Arguments: is_hex, use_length_tag, separator, length_tag_name
        self.whiten = lora_sdr.whitening(False, False)
        # Header adds LoRa explicit header with payload length/CRC
        self.header = lora_sdr.header(False, True, cr)
        self.add_crc = lora_sdr.add_crc(True)
        self.hamming_enc = lora_sdr.hamming_enc(cr, sf)
        self.interleaver = lora_sdr.interleaver(cr, sf, ldro, int(bw_hz))
        self.gray_demap = lora_sdr.gray_demap(sf)
        self.mod = lora_sdr.modulate(sf, int(samp_rate_hz), int(bw_hz), [sync_word], 0, preamb_len)
        self.sink = blocks.file_sink(gr.sizeof_gr_complex, out_iq_path, False)
        self.sink.set_unbuffered(True)

        self.connect(self.src, self.whiten)
        self.connect(self.whiten, self.header)
        self.connect(self.header, self.add_crc)
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
    ap.add_argument("--timeout", type=float, default=20.0, help="Max seconds to wait for IQ generation")
    args = ap.parse_args(argv)

    print(f"[gr-generate] sf={args.sf} cr={args.cr} payload={args.payload} out={args.out}", flush=True)
    def try_direct_chain():
        tb = LoraTxOnly(args.sf, args.cr, args.payload, args.out,
                        bw_hz=args.bw, samp_rate_hz=args.samp_rate, preamb_len=args.preamble_len)
        try:
            tb.start()
            # Poll output file size until > 0 or timeout
            import os, time
            start = time.time()
            last_sz = -1
            while True:
                try:
                    sz = os.path.getsize(args.out)
                except FileNotFoundError:
                    sz = 0
                if sz != last_sz:
                    print(f"[gr-generate] direct-chain out size={sz}", flush=True)
                    last_sz = sz
                if sz > 0:
                    break
                if time.time() - start > args.timeout:
                    print("[gr-generate] direct-chain timeout", flush=True)
                    return 2
                time.sleep(0.1)
            time.sleep(0.25)
        finally:
            try:
                tb.stop(); tb.wait()
            except Exception as e:
                print(f"[gr-generate] direct stop/wait: {e}", flush=True)
        return 0

    rc = try_direct_chain()
    if rc == 0:
        return 0

    # Fallback: use reference tx_rx_simulation top block and tap IQ from modulator
    print("[gr-generate] falling back to tx_rx_simulation tap", flush=True)
    import os, time, pathlib
    root = pathlib.Path(__file__).resolve().parent.parent
    sys.path.insert(0, str(root / 'external' / 'gr_lora_sdr' / 'apps' / 'simulation' / 'flowgraph'))
    try:
        import gnuradio.lora_sdr as _lora_sdr_mod
        _orig_whiten = _lora_sdr_mod.whitening
        def _whiten_compat(*a, **kw):
            # Accept legacy 1-arg form whitening(False)
            if len(a) == 1 and isinstance(a[0], bool):
                return _orig_whiten(a[0], False)
            return _orig_whiten(*a, **kw)
        _lora_sdr_mod.whitening = _whiten_compat
        import tx_rx_simulation as txrx
    except Exception as e:
        print(f"[gr-generate] failed importing tx_rx_simulation: {e}", flush=True)
        return 3
    pay_len = os.path.getsize(args.payload)
    cr_api = args.cr - 40
    tb = txrx.tx_rx_simulation(args.payload, '/tmp/rx_payload.bin', '/tmp/rx_crc.bin',
                               False, False, 100, int(args.samp_rate), int(args.bw), 868.1,
                               int(args.sf), int(cr_api), int(pay_len), 0, 1 if args.sf>6 else 0,
                               int(args.preamble_len))
    iq_sink = blocks.file_sink(gr.sizeof_gr_complex, args.out, False)
    iq_sink.set_unbuffered(True)
    tb.iq_sink = iq_sink
    tb.connect((tb.lora_sdr_modulate_0, 0), (tb.iq_sink, 0))
    try:
        tb.start()
        start = time.time(); last_sz = -1
        while True:
            try:
                sz = os.path.getsize(args.out)
            except FileNotFoundError:
                sz = 0
            if sz != last_sz:
                print(f"[gr-generate] txrx-tap out size={sz}", flush=True)
                last_sz = sz
            if sz > 0:
                break
            if time.time() - start > args.timeout:
                print("[gr-generate] txrx-tap timeout", flush=True)
                return 4
            time.sleep(0.1)
        time.sleep(0.25)
    finally:
        try:
            tb.stop(); tb.wait()
        except Exception as e:
            print(f"[gr-generate] txrx stop/wait: {e}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
