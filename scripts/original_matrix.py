#!/usr/bin/env python3
import argparse, os, sys, time, json, random, string
from pathlib import Path

import os
# Try to reduce shared memory pressure in headless runs
os.environ.setdefault('GR_VMCIRCBUF_DISABLE_SHM', '1')
try:
    from gnuradio import gr, blocks, channels
    import gnuradio.lora_sdr as lora_sdr
except Exception as e:
    print(f"[error] import GNURadio/lora_sdr failed: {e}", file=sys.stderr)


def rand_ascii(n: int, seed: int) -> str:
    rnd = random.Random(seed)
    alphabet = string.ascii_letters + string.digits + ' _-:.,/+'
    return ''.join(rnd.choice(alphabet) for _ in range(n))


class OrigHarness(gr.top_block):
    def __init__(self, sf: int, cr_lora: int, text: str, bw: int, sr: int, preamb: int = 8, sync: int = 0x12):
        gr.top_block.__init__(self, "orig_harness", catch_exceptions=True)
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
        # Ensure upstream buffer can satisfy frame_sync requirements at high SF
        try:
            min_buf = int((2**sf) * (sr / bw) * 1.1)
            self.chan.set_min_output_buffer(min_buf)
        except Exception:
            pass
        # Throttle to stabilize scheduling in headless runs
        # Use moderate throttle to stabilize scheduling without large buffers
        self.throttle = blocks.throttle(gr.sizeof_gr_complex, int(sr*2), True)
        self.connect(self.tx, self.throttle)
        self.connect(self.throttle, self.chan)
        self.connect(self.chan, self.rx)
        self.msg_connect(self.msg_src, 'strobe', self.tx, 'in')
        self.msg_connect(self.rx, 'out', self.msg_dbg, 'store')


def run_case(sf, cr, bw, length, seed=1234, timeout=5.0):
    text = rand_ascii(length, seed)
    sr = 4 * bw
    tb = OrigHarness(sf, cr, text, bw, sr)
    ok = False; recv = ''
    try:
        tb.start()
        start = time.time()
        while True:
            try:
                if tb.msg_dbg.num_messages() > 0:
                    m = tb.msg_dbg.get_message(0)
                    import pmt
                    if pmt.is_symbol(m):
                        recv = pmt.symbol_to_string(m)
                    elif pmt.is_pair(m):
                        md = pmt.cdr(m)
                        if pmt.is_u8vector(md):
                            recv = bytes(pmt.u8vector_elements(md)).decode('ascii','ignore')
            except Exception:
                pass
            if len(recv) >= length:
                break
            if time.time() - start > timeout:
                break
            time.sleep(0.05)
    finally:
        try: tb.stop(); tb.wait()
        except Exception: pass
        # Ensure buffers are released between runs
        import gc; del tb; gc.collect(); time.sleep(0.05)
    ok = (recv[:length] == text)
    return ok, text, recv[:length]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='reports_orig')
    ap.add_argument('--sfs', default='7,8,9,10,11,12')
    ap.add_argument('--crs', default='45,46,47,48')
    ap.add_argument('--bws', default='125000,250000,500000')
    ap.add_argument('--lengths', default='14,16,48')
    ap.add_argument('--reps', type=int, default=1)
    ap.add_argument('--timeout', type=float, default=6.0)
    ap.add_argument('--chunk', action='store_true', help='Run each SF group in a separate subprocess (prevents SHM exhaustion)')
    ap.add_argument('--chunk-pause', type=float, default=0.2, help='Pause seconds between chunked runs')
    ap.add_argument('--_chunk_run', action='store_true', help=argparse.SUPPRESS)
    args = ap.parse_args()

    build = Path(__file__).resolve().parents[1] / 'build'
    outdir = build / args.out
    outdir.mkdir(parents=True, exist_ok=True)
    csv = outdir / 'orig_validate.csv'
    # Append if file already exists; write header only once
    need_header = True
    if csv.exists():
        try:
            if csv.stat().st_size > 0:
                need_header = False
        except Exception:
            need_header = True
    with csv.open('a' if not need_header else 'w') as f:
        if need_header:
            f.write('sf,cr,bw,len,ok,reason\n')

    sfs = [int(x) for x in args.sfs.split(',') if x]
    crs = [int(x) for x in args.crs.split(',') if x]
    bws = [int(x) for x in args.bws.split(',') if x]
    lens= [int(x) for x in args.lengths.split(',') if x]

    # If chunk mode is enabled and this is the parent invocation, re-launch per SF group
    if args.chunk and not args._chunk_run and len(sfs) > 1:
        for sf in sfs:
            cmd = [sys.executable, __file__, '--sfs', str(sf), '--crs', args.crs, '--bws', args.bws,
                   '--lengths', args.lengths, '--reps', str(args.reps), '--timeout', str(args.timeout),
                   '--out', args.out, '--_chunk_run']
            env = os.environ.copy()
            try:
                print(f"[chunk] running SF={sf}")
                subprocess = __import__('subprocess')
                res = subprocess.run(cmd, env=env)
            except Exception as e:
                print(f"[chunk] failed for SF={sf}: {e}", file=sys.stderr)
            time.sleep(args.chunk_pause)
        # Summary generation repeats below; continue to summary
    else:
        for sf in sfs:
            for cr in crs:
                for bw in bws:
                    for ln in lens:
                        for r in range(args.reps):
                            try:
                                # Run each case in a subprocess to avoid SHM accumulation
                                import subprocess, json
                                cmd = [sys.executable, str(Path(__file__).resolve().parents[0] / 'original_case.py'),
                                       '--sf', str(sf), '--cr', str(cr), '--bw', str(bw), '--len', str(ln),
                                       '--seed', str(1000+r), '--timeout', str(args.timeout)]
                                res = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                                j = {}
                                try:
                                    j = json.loads(res.stdout.strip()) if res.stdout.strip() else {}
                                except Exception:
                                    j = {}
                                ok = bool(j.get('ok', False))
                                reason = '' if ok else ('no_output_or_mismatch' if res.returncode!=2 else 'gnuradio_missing')
                            except MemoryError:
                                ok = False; reason = 'alloc_failed'
                            except Exception:
                                ok = False; reason = 'exception'
                            with csv.open('a') as f:
                                f.write(f"{sf},{cr},{bw},{ln},{1 if ok else 0},{reason}\n")
                            print(f"[orig] sf={sf} cr={cr} bw={bw} len={ln} ok={ok}")
                            time.sleep(0.05)
    # Generate a simple README summary
    rd = outdir / 'README.md'
    try:
        rows = csv.read_text().strip().splitlines()[1:]
        total = len(rows)
        ok = sum(1 for r in rows if r.split(',')[4] == '1')
        by_sfcr = {}
        for r in rows:
            sf,cr,_,_,okv,_ = r.split(',')
            key = (sf,cr)
            s = by_sfcr.setdefault(key, {'tot':0,'ok':0})
            s['tot'] += 1
            s['ok'] += 1 if okv=='1' else 0
        with rd.open('w') as f:
            f.write('# Original (hierarchical) TX→RX Validation\n\n')
            f.write(f'- Cases: {total}\n')
            rate = (100.0*ok/total) if total else 0.0
            f.write(f'- Success: {ok}/{total} ({rate:.1f}%)\n\n')
            f.write('## By SF/CR\n')
            for (sf,cr), v in sorted(by_sfcr.items(), key=lambda x:(int(x[0][0]), int(x[0][1]))):
                srate = 100.0*v['ok']/v['tot'] if v['tot'] else 0.0
                f.write(f"- SF={sf} CR={cr}: {v['ok']}/{v['tot']} ({srate:.1f}%)\n")
    except Exception:
        pass
    print(f"Done. CSV: {csv}", file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
