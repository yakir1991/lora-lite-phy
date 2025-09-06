#!/usr/bin/env python3
import argparse, os, sys, time, json, random, string
from pathlib import Path

import os
# Try to reduce shared memory pressure in headless runs (cover both variants)
os.environ.setdefault('GR_VMCIRCBUF_DISABLE_SHM', '1')
os.environ.setdefault('GR_DONT_USE_SHARED_MEMORY', '1')
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
    def __init__(self, sf: int, cr_lora: int, text: str, bw: int, sr: int, snr_db: float = 0.0, preamb: int = 8, sync: int = 0x12):
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
        self.chan = channels.channel_model(noise_voltage=(10**(-float(snr_db)/20.0)), frequency_offset=0.0, epsilon=1.0,
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


def run_case(sf, cr, bw, length, seed=1234, timeout=5.0, snr_db: float = 0.0):
    text = rand_ascii(length, seed)
    sr = 4 * bw
    tb = OrigHarness(sf, cr, text, bw, sr, snr_db=snr_db)
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
    ap.add_argument('--snrs', default='0,5,10,15', help='Comma-separated SNR values in dB (e.g., "0,5,10,15")')
    # Convenience options for quicker runs
    ap.add_argument('--quick', action='store_true', help='Run a small sanity subset (sf=7, cr=45, bw=125k, len=16)')
    ap.add_argument('--max-cases', type=int, default=0, help='Process at most this many cases (0 = no limit)')
    ap.add_argument('--chunk', action='store_true', help='Run in subprocess chunks to avoid GNURadio/OOT memory buildup')
    ap.add_argument('--chunk-by', choices=['sf','cr','bw','len','snr'], default='sf',
                    help='When --chunk is set, split the matrix by this dimension and run each value in a subprocess')
    ap.add_argument('--chunk-pause', type=float, default=0.2, help='Pause seconds between chunked runs')
    ap.add_argument('--_chunk_run', action='store_true', help=argparse.SUPPRESS)
    args = ap.parse_args()

    build = Path(__file__).resolve().parents[1] / 'build'
    outdir = build / args.out
    outdir.mkdir(parents=True, exist_ok=True)
    # prepare CSV with SNR column; if existing header mismatches, write to v2 file
    expected_header = 'sf,cr,bw,len,snr,ok,reason'
    csv = outdir / 'orig_validate.csv'
    need_header = True
    if csv.exists() and csv.stat().st_size > 0:
        try:
            first = csv.read_text().splitlines()[0].strip()
            if first == expected_header:
                need_header = False
            else:
                csv = outdir / 'orig_validate_v2.csv'
                need_header = not (csv.exists() and csv.stat().st_size > 0 and csv.read_text().splitlines()[0].strip() == expected_header)
        except Exception:
            pass
    with csv.open('a' if not need_header else 'w') as f:
        if need_header:
            f.write(expected_header + '\n')

    # Resolve matrices
    if args.quick:
        sfs = [7]; crs = [45]; bws = [125000]; lens = [16]
    else:
        sfs = [int(x) for x in args.sfs.split(',') if x]
        crs = [int(x) for x in args.crs.split(',') if x]
        bws = [int(x) for x in args.bws.split(',') if x]
        lens= [int(x) for x in args.lengths.split(',') if x]
    snrs = [float(x) for x in args.snrs.split(',') if str(x).strip()!='']

    # Report planned number of cases
    total_cases = len(sfs) * len(crs) * len(bws) * len(lens) * len(snrs) * int(args.reps)
    print(f"[orig] starting matrix: {total_cases} case(s)", flush=True)

    # If quick mode: run a single case inline (fast feedback)
    if args.quick:
        sf, cr, bw, ln = sfs[0], crs[0], bws[0], lens[0]
        snr0 = snrs[0] if snrs else 0.0
        print(f"[orig] inline quick run sf={sf} cr={cr} bw={bw} len={ln} snr={snr0}", flush=True)
        ok, sent, recv = run_case(sf, cr, bw, ln, seed=1000, timeout=args.timeout, snr_db=snr0)
        with csv.open('a') as f:
            f.write(f"{sf},{cr},{bw},{ln},{snr0},{1 if ok else 0},{'' if ok else 'mismatch_or_timeout'}\n")
        try:
            from summarize_matrix import load_rows, summarize, write_readme
        except Exception:
            sys.path.insert(0, str(Path(__file__).parent))
            from summarize_matrix import load_rows, summarize, write_readme
        rows = load_rows(csv)
        sm = summarize(rows)
        write_readme(outdir, sm)
        print(f"[orig] quick done ok={ok}", flush=True)
        print(f"Done. CSV: {csv}", file=sys.stderr)
        return 0

    # If chunk mode is enabled and this is the parent invocation, re-launch per selected group
    processed = 0
    try:
        if args.chunk and not args._chunk_run:
            # Select values and CLI arg name for the chosen chunk dimension
            dim = args.chunk_by
            chunks = []
            if dim == 'sf':
                for v in sfs:
                    chunks.append(['--sfs', str(v), '--crs', args.crs, '--bws', args.bws,
                                   '--lengths', args.lengths, '--snrs', args.snrs])
            elif dim == 'cr':
                for v in crs:
                    chunks.append(['--sfs', args.sfs, '--crs', str(v), '--bws', args.bws,
                                   '--lengths', args.lengths, '--snrs', args.snrs])
            elif dim == 'bw':
                for v in bws:
                    chunks.append(['--sfs', args.sfs, '--crs', args.crs, '--bws', str(v),
                                   '--lengths', args.lengths, '--snrs', args.snrs])
            elif dim == 'len':
                for v in lens:
                    chunks.append(['--sfs', args.sfs, '--crs', args.crs, '--bws', args.bws,
                                   '--lengths', str(v), '--snrs', args.snrs])
            elif dim == 'snr':
                for v in snrs:
                    chunks.append(['--sfs', args.sfs, '--crs', args.crs, '--bws', args.bws,
                                   '--lengths', args.lengths, '--snrs', str(v)])
            else:
                chunks = []

            for ch in chunks:
                cmd = [sys.executable, __file__] + ch + [
                    '--reps', str(args.reps), '--timeout', str(args.timeout), '--out', args.out, '--_chunk_run']
                if args.quick:
                    cmd.append('--quick')
                if args.max_cases:
                    cmd += ['--max-cases', str(args.max_cases)]
                env = os.environ.copy()
                try:
                    print(f"[chunk] running {dim}={' '.join(ch[1:2])}")
                    subprocess = __import__('subprocess')
                    res = subprocess.run(cmd, env=env)
                except Exception as e:
                    print(f"[chunk] failed for {dim} group {ch}: {e}", file=sys.stderr)
                time.sleep(args.chunk_pause)
            # Summary generation repeats below; continue to summary
        else:
            for sf in sfs:
                for cr in crs:
                    for bw in bws:
                        for ln in lens:
                            for snr in snrs:
                                for r in range(args.reps):
                                    if args.max_cases and processed >= args.max_cases:
                                        raise KeyboardInterrupt()
                                    print(f"[orig] run sf={sf} cr={cr} bw={bw} len={ln} snr={snr} rep={r}", flush=True)
                                    ok = False; reason = ''
                                    try:
                                        ok, sent, recv = run_case(sf, cr, bw, ln, seed=(1000+r), timeout=args.timeout, snr_db=snr)
                                        reason = '' if ok else 'mismatch_or_timeout'
                                    except MemoryError:
                                        ok = False; reason = 'alloc_failed'
                                    except KeyboardInterrupt:
                                        ok = False; reason = 'interrupted'
                                        with csv.open('a') as f:
                                            f.write(f"{sf},{cr},{bw},{ln},{snr},{1 if ok else 0},{reason}\n")
                                        processed += 1
                                        raise
                                    except Exception:
                                        ok = False; reason = 'exception'
                                    with csv.open('a') as f:
                                        f.write(f"{sf},{cr},{bw},{ln},{snr},{1 if ok else 0},{reason}\n")
                                    processed += 1
                                    print(f"[orig] done sf={sf} cr={cr} bw={bw} len={ln} snr={snr} ok={ok}", flush=True)
                                    time.sleep(0.05)
    except KeyboardInterrupt:
        print("[orig] interrupted by user", file=sys.stderr)
    finally:
        try:
            from summarize_matrix import load_rows, summarize, write_readme
        except Exception:
            sys.path.insert(0, str(Path(__file__).parent))
            from summarize_matrix import load_rows, summarize, write_readme
        rows = load_rows(csv)
        sm = summarize(rows)
        write_readme(outdir, sm)
    print(f"Done. CSV: {csv}", file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
