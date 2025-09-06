#!/usr/bin/env python3
import argparse, os, time, random, string, json
from gnuradio import gr, blocks, channels
from gnuradio import eng_notation
from gnuradio.fft import window
import gnuradio.lora_sdr as lora_sdr


def rand_ascii_payload(n: int, seed: int) -> str:
    rnd = random.Random(seed)
    alphabet = string.ascii_letters + string.digits + ' _-:.,/+'
    return ''.join(rnd.choice(alphabet) for _ in range(n))


class OrigEndToEnd(gr.top_block):
    def __init__(self, sf: int, cr_lora: int, payload_ascii: str,
                 bw_hz: int, samp_rate_hz: int,
                 snr_db: float, clk_ppm: float,
                 preamb_len: int = 8, sync_word=0x12,
                 out_iq_path: str | None = None,
                 out_payload_path: str | None = None,
                 enable_throttle: bool = True,
                 strobe_ms: int = 1000):
        gr.top_block.__init__(self, "orig_e2e", catch_exceptions=True)

        # Map LoRa CR (45,46,47,48) -> API (1..4)
        cr_map = {45:1, 46:2, 47:3, 48:4}
        cr_api = cr_map.get(cr_lora, 1)
        ldro = 2  # align with hierarchical defaults

        # TX/RX hierarchical blocks
        self.tx = lora_sdr.lora_sdr_lora_tx(
            bw=int(bw_hz), cr=int(cr_api), has_crc=True, impl_head=False,
            samp_rate=int(samp_rate_hz), sf=int(sf), ldro_mode=ldro,
            frame_zero_padd=int(20 * (2**sf) * samp_rate_hz / bw_hz), sync_word=[int(sync_word)]
        )
        self.rx = lora_sdr.lora_sdr_lora_rx(
            bw=int(bw_hz), cr=int(cr_api), has_crc=True, impl_head=False, pay_len=255,
            samp_rate=int(samp_rate_hz), sf=int(sf), sync_word=[int(sync_word)],
            soft_decoding=True, ldro_mode=ldro, print_rx=[True, True]
        )

        # Channel model
        self.chan = channels.channel_model(
            noise_voltage=(10**(-snr_db/20.0)),
            frequency_offset=(0.0),
            epsilon=(1.0 + float(clk_ppm)*1e-6),
            taps=[1.0+0.0j],
            noise_seed=1,
            block_tags=True
        )
        self.chan.set_min_output_buffer(int((2**sf + 2) * samp_rate_hz / bw_hz))
        # Throttle between TX and channel to emulate realtime pacing like examples
        self.throttle = blocks.throttle(gr.sizeof_gr_complex, int(samp_rate_hz*10), True) if enable_throttle else None

        # Message input (ascii), sinks
        import pmt
        self.msg = blocks.message_strobe(pmt.intern(payload_ascii), int(strobe_ms))
        # Optional message payload-increment helper to mimic example behavior
        try:
            self.id_inc = lora_sdr.payload_id_inc(':')
            self.msg_connect(self.msg, 'strobe', self.id_inc, 'msg_in')
            self.msg_connect(self.id_inc, 'msg_out', self.msg, 'set_msg')
        except Exception:
            self.id_inc = None
        if out_iq_path:
            self.iq_sink = blocks.file_sink(gr.sizeof_gr_complex, out_iq_path, False)
            self.iq_sink.set_unbuffered(True)
        else:
            self.iq_sink = None
        self.msg_dbg = blocks.message_debug()
        # Always capture RX stream to a file to allow robust verification
        self.out_payload_path = out_payload_path or '/tmp/orig_e2e_rx.bin'
        self.payload_sink = blocks.file_sink(gr.sizeof_char, self.out_payload_path, False)
        self.payload_sink.set_unbuffered(True)

        # Connections
        self.msg_connect(self.msg, 'strobe', self.tx, 'in')
        if self.throttle:
            self.connect(self.tx, self.throttle)
            self.connect(self.throttle, self.chan)
        else:
            self.connect(self.tx, self.chan)
        if self.iq_sink:
            self.connect(self.tx, self.iq_sink)  # tap clean IQ too
        self.connect(self.chan, self.rx)
        # Capture payload as message
        # Capture string payload messages from RX 'out'
        self.msg_connect(self.rx, 'out', self.msg_dbg, 'store')
        self.connect(self.rx, self.payload_sink)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sf', type=int, required=True)
    ap.add_argument('--cr', type=int, required=True, help='LoRa CR as 45/46/47/48')
    ap.add_argument('--len', type=int, required=True, help='ASCII payload length')
    ap.add_argument('--text', default='', help='Exact ASCII text to send (overrides --len)')
    ap.add_argument('--bw', type=int, default=125000)
    ap.add_argument('--samp-rate', type=int, default=0, help='Defaults to 4*bw if 0')
    ap.add_argument('--snr', type=float, default=0.0)
    ap.add_argument('--clk-ppm', type=float, default=0.0)
    ap.add_argument('--preamble-len', type=int, default=8)
    ap.add_argument('--sync', type=lambda x:int(x,0), default=0x12)
    ap.add_argument('--seed', type=int, default=1234)
    ap.add_argument('--out-iq', default='')
    ap.add_argument('--out-payload', default='')
    ap.add_argument('--timeout', type=float, default=10.0)
    ap.add_argument('--no-throttle', action='store_true')
    ap.add_argument('--strobe-ms', type=int, default=1000)
    args = ap.parse_args()

    try:
        if args.text:
            payload_ascii = args.text
        else:
            # Use a demo-friendly default like the official example if len matches, else random
            demo = 'Hello world: 0'
            payload_ascii = demo if len(demo) == args.len else rand_ascii_payload(args.len, args.seed)
        sr = args.samp_rate if args.samp_rate > 0 else (4*args.bw)
        tb = OrigEndToEnd(args.sf, args.cr, payload_ascii,
                          args.bw, sr, args.snr, args.clk_ppm,
                          args.preamble_len, args.sync,
                          out_iq_path=(args.out_iq or None),
                          out_payload_path=(args.out_payload or None),
                          enable_throttle=(not args.no_throttle),
                          strobe_ms=args.strobe_ms)
        tb.start()
        start = time.time()
        got = ''
        # Poll both message queue and stream file until substring found
        while True:
            # Stream file check
            try:
                with open(tb.out_payload_path,'rb') as f:
                    data = f.read()
                    stream_txt = data.decode('ascii', errors='ignore')
                    if payload_ascii in stream_txt:
                        got = payload_ascii
            except FileNotFoundError:
                pass
            # Message queue check
            try:
                import pmt
                while tb.msg_dbg.num_messages() > 0:
                    m = tb.msg_dbg.get_message(0)
                    try:
                        tb.msg_dbg.delete_head_nowait()
                    except Exception:
                        pass
                    if pmt.is_symbol(m):
                        msg = pmt.symbol_to_string(m)
                        if payload_ascii in msg:
                            got = payload_ascii
                    elif pmt.is_pair(m):
                        md = pmt.cdr(m)
                        if pmt.is_u8vector(md):
                            msg = bytes(pmt.u8vector_elements(md)).decode('ascii','ignore')
                            if payload_ascii in msg:
                                got = payload_ascii
            except Exception:
                pass
            if got == payload_ascii:
                break
            if time.time() - start > args.timeout:
                break
            time.sleep(0.05)
    finally:
        try:
            tb.stop(); tb.wait()
        except Exception:
            pass

    ok = (got[:len(payload_ascii)] == payload_ascii)
    print(json.dumps({'ok': ok, 'sent': payload_ascii, 'recv': got[:len(payload_ascii)]}))
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
