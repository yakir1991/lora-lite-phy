#!/usr/bin/env python3
import argparse, os, sys, time, json, random, string

os.environ.setdefault('GR_VMCIRCBUF_DISABLE_SHM', '1')

from gnuradio import gr, blocks, channels
import gnuradio.lora_sdr as lora_sdr


def rand_ascii(n: int, seed: int) -> str:
    rnd = random.Random(seed)
    alphabet = string.ascii_letters + string.digits + ' _-:.,/+'
    return ''.join(rnd.choice(alphabet) for _ in range(n))


class Harness(gr.top_block):
    def __init__(self, sf, cr_lora, text, bw, sr, sync=0x12):
        gr.top_block.__init__(self, "orig_case", catch_exceptions=True)
        cr_map = {45:1,46:2,47:3,48:4}
        cr_api = cr_map.get(cr_lora, 1)
        ldro = 2
        self.tx = lora_sdr.lora_sdr_lora_tx(bw=int(bw), cr=int(cr_api), has_crc=True, impl_head=False,
                                            samp_rate=int(sr), sf=int(sf), ldro_mode=ldro,
                                            frame_zero_padd=int(20 * (2**sf) * sr / bw), sync_word=[int(sync)])
        self.rx = lora_sdr.lora_sdr_lora_rx(bw=int(bw), cr=int(cr_api), has_crc=True, impl_head=False, pay_len=255,
                                            samp_rate=int(sr), sf=int(sf), sync_word=[int(sync)],
                                            soft_decoding=True, ldro_mode=ldro, print_rx=[False, False])
        import pmt
        self.msg_src = blocks.message_strobe(pmt.intern(text), 500)
        self.msg_dbg = blocks.message_debug()
        self.chan = channels.channel_model(noise_voltage=0.0, frequency_offset=0.0, epsilon=1.0,
                                           taps=[1.0+0.0j], noise_seed=1, block_tags=True)
        try:
            min_buf = int((2**sf) * (sr / bw) * 1.1)
            self.chan.set_min_output_buffer(min_buf)
        except Exception:
            pass
        self.throttle = blocks.throttle(gr.sizeof_gr_complex, int(sr*2), True)
        self.connect(self.tx, self.throttle)
        self.connect(self.throttle, self.chan)
        self.connect(self.chan, self.rx)
        self.msg_connect(self.msg_src, 'strobe', self.tx, 'in')
        self.msg_connect(self.rx, 'out', self.msg_dbg, 'store')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sf', type=int, required=True)
    ap.add_argument('--cr', type=int, required=True)
    ap.add_argument('--bw', type=int, required=True)
    ap.add_argument('--len', type=int, required=True)
    ap.add_argument('--seed', type=int, default=1000)
    ap.add_argument('--timeout', type=float, default=8.0)
    args = ap.parse_args()

    text = rand_ascii(args.len, args.seed)
    sr = 4 * args.bw
    tb = Harness(args.sf, args.cr, text, args.bw, sr)
    ok = False; recv = ''
    try:
        tb.start()
        start = time.time()
        while True:
            try:
                import pmt
                while tb.msg_dbg.num_messages() > 0:
                    m = tb.msg_dbg.get_message(0)
                    if pmt.is_symbol(m):
                        recv = pmt.symbol_to_string(m)
                    elif pmt.is_pair(m):
                        md = pmt.cdr(m)
                        if pmt.is_u8vector(md):
                            recv = bytes(pmt.u8vector_elements(md)).decode('ascii','ignore')
                    try:
                        tb.msg_dbg.delete_head_nowait()
                    except Exception:
                        pass
            except Exception:
                pass
            if len(recv) >= args.len:
                break
            if time.time() - start > args.timeout:
                break
            time.sleep(0.05)
    finally:
        try:
            tb.stop(); tb.wait()
        except Exception:
            pass

    ok = (recv[:args.len] == text)
    print(json.dumps({'ok': ok, 'sent': text, 'recv': recv[:args.len]}))
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
