#!/usr/bin/env python3
import argparse
import sys, os, json


def run_rx_only(in_iq: str, sf: int, cr_lora: int, bw_hz: int, samp_rate_hz: int, pay_len: int, timeout: float,
                out_rx_payload: str) -> dict:
    from gnuradio import gr, blocks
    # Use hierarchical RX to match reference behavior; will print CRC but not emit boolean
    from gnuradio.lora_sdr.lora_sdr_lora_rx import lora_sdr_lora_rx

    if cr_lora not in (45, 46, 47, 48):
        raise ValueError('cr_lora must be 45/46/47/48')
    cr_api = cr_lora - 40

    class TB(gr.top_block):
        pass

    tb = TB('orig_rx_only')
    src = blocks.file_source(gr.sizeof_gr_complex, in_iq, False)
    rx  = lora_sdr_lora_rx(center_freq=int(868.1e6), bw=int(bw_hz), cr=int(cr_api), has_crc=True,
                               impl_head=False, pay_len=int(pay_len), samp_rate=int(samp_rate_hz), sf=int(sf),
                               sync_word=[0x12], soft_decoding=False, ldro_mode=2, print_rx=[False, False])
    sink_rx_payload = blocks.file_sink(gr.sizeof_char, out_rx_payload, False)
    tb.connect(src, rx)
    tb.connect(rx, sink_rx_payload)

    try:
        tb.start()
        import time
        start = time.time(); last_sz = -1
        while True:
            try:
                sz = os.path.getsize(out_rx_payload)
            except FileNotFoundError:
                sz = 0
            if sz != last_sz:
                last_sz = sz
            if sz > 0:
                break
            if time.time() - start > timeout:
                break
            time.sleep(0.05)
    finally:
        try:
            tb.stop(); tb.wait()
        except Exception:
            pass

    # No explicit CRC boolean; just report payload length
    return {'ok': None, 'reason': 'n/a',
            'rx_payload_len': os.path.getsize(out_rx_payload) if os.path.exists(out_rx_payload) else 0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in-iq', required=True)
    ap.add_argument('--sf', type=int, required=True)
    ap.add_argument('--cr', type=int, required=True)
    ap.add_argument('--bw', type=int, default=125000)
    ap.add_argument('--samp-rate', type=int, default=500000)
    ap.add_argument('--pay-len', type=int, required=True)
    ap.add_argument('--timeout', type=float, default=10.0)
    ap.add_argument('--out-rx-payload', required=True)
    args = ap.parse_args()

    try:
        import gnuradio, gnuradio.lora_sdr  # noqa: F401
    except Exception as e:
        print(json.dumps({'ok': False, 'reason': f'gnuradio_missing: {e}'}))
        return 2

    try:
        res = run_rx_only(args.in_iq, args.sf, args.cr, args.bw, args.samp_rate, args.pay_len, args.timeout,
                          args.out_rx_payload)
        print(json.dumps(res))
        return 0
    except Exception as e:
        print(json.dumps({'ok': False, 'reason': f'exception: {e}'}))
        return 3


if __name__ == '__main__':
    sys.exit(main())
