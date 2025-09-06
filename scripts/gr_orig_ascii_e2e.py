#!/usr/bin/env python3
# WARNING:
# This flowgraph-based runner (tx_rx_simulation + file_source) is known to be unreliable
# in headless automation because whitening/header framing via file_source lacks the
# proper message/tags lifecycle. Prefer hierarchical TX/RX runner:
#   scripts/gr_original_e2e.py
# Keeping this file for reference; results may be false (ok=false) even on valid setups.
import argparse, os, time, json, random, string

def rand_ascii(n, seed):
    rnd = random.Random(seed)
    alphabet = string.ascii_letters + string.digits + ' _-:.,/+'
    return ''.join(rnd.choice(alphabet) for _ in range(n))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sf', type=int, required=True)
    ap.add_argument('--cr', type=int, required=True)
    ap.add_argument('--len', type=int, required=True)
    ap.add_argument('--bw', type=int, default=125000)
    ap.add_argument('--samp-rate', type=int, default=0)
    ap.add_argument('--snr', type=float, default=0.0)
    ap.add_argument('--clk-ppm', type=float, default=0.0)
    ap.add_argument('--preamble-len', type=int, default=8)
    ap.add_argument('--timeout', type=float, default=10.0)
    ap.add_argument('--seed', type=int, default=1234)
    ap.add_argument('--out-iq', default='/tmp/gr_orig_ascii_iq.bin')
    ap.add_argument('--rx-payload', default='/tmp/gr_orig_ascii_rx.bin')
    ap.add_argument('--rx-crc', default='/tmp/gr_orig_ascii_crc.bin')
    args = ap.parse_args()

    try:
        import sys, pathlib
        root = pathlib.Path(__file__).resolve().parents[1]
        sys.path.insert(0, str(root / 'external' / 'gr_lora_sdr' / 'apps' / 'simulation' / 'flowgraph'))
        import gnuradio.lora_sdr as _lora_mod
        _orig_whiten = _lora_mod.whitening
        def _whiten_compat(*a, **kw):
            # Force hex mode for single-arg legacy calls so we can feed hex CSV via file_source
            if len(a) == 1 and isinstance(a[0], bool):
                return _orig_whiten(True, False, ',', 'packet_len')
            return _orig_whiten(*a, **kw)
        _lora_mod.whitening = _whiten_compat
        import tx_rx_simulation as txrx
        from gnuradio import blocks, gr
        import gnuradio
    except Exception as e:
        print(json.dumps({'ok': False, 'reason': f'import_failed:{e}'}))
        return 2

    text = rand_ascii(args.len, args.seed)
    # Prepare HEX CSV frame compatible with whitening(True, use_length_tag=False)
    hex_csv = ','.join(f"{ord(c):02x}" for c in text) + ','
    pay_path = '/tmp/gr_orig_ascii_payload.txt'
    with open(pay_path, 'w') as f:
        f.write(hex_csv)

    sr = args.samp_rate if args.samp_rate > 0 else (4*args.bw)
    cr_map = {45:1,46:2,47:3,48:4}
    cr_api = cr_map.get(args.cr, 1)
    tb = txrx.tx_rx_simulation(pay_path, args.rx_payload, args.rx_crc,
                               False, False, float(args.snr), int(sr), int(args.bw), 868.1,
                               int(args.sf), int(cr_api), int(args.len), float(args.clk_ppm), 1 if args.sf>6 else 0,
                               int(args.preamble_len))
    # Tap clean IQ
    iq_sink = blocks.file_sink(gr.sizeof_gr_complex, args.out_iq, False)
    iq_sink.set_unbuffered(True)
    tb.connect((tb.lora_sdr_modulate_0, 0), (iq_sink, 0))

    ok = False
    try:
        tb.start()
        start = time.time()
        while True:
            try:
                crc_sz = os.path.getsize(args.rx_crc)
            except FileNotFoundError:
                crc_sz = 0
            if crc_sz > 0:
                break
            if time.time() - start > args.timeout:
                break
            time.sleep(0.05)
    finally:
        try:
            tb.stop(); tb.wait()
        except Exception:
            pass

    # Check payload
    got = ''
    if os.path.exists(args.rx_payload):
        try:
            got = open(args.rx_payload,'rb').read().decode('ascii', errors='ignore')
        except Exception:
            got = ''
    ok = (got[:len(text)] == text)
    print(json.dumps({'ok': ok, 'sent': text, 'recv': got[:len(text)]}))
    return 0 if ok else 1

if __name__ == '__main__':
    raise SystemExit(main())
