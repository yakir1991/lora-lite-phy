#!/usr/bin/env python3
import argparse
import sys, os, time, json, pathlib


class OrigTxRx:
    def __init__(self, sf: int, cr_api: int, payload_path: str,
                 out_rx_payload: str, out_rx_crc: str,
                 bw_hz: int, samp_rate_hz: int,
                 snr_db: float, clk_ppm: float, preamb_len: int):
        import importlib, sys as _sys
        import gnuradio.lora_sdr as lora_sdr
        root = pathlib.Path(__file__).resolve().parents[1]
        _sys.path.insert(0, str(root / 'external' / 'gr_lora_sdr' / 'apps' / 'simulation' / 'flowgraph'))
        # Back-compat for whitening one-arg signature used by reference flowgraph
        try:
            _orig_whiten = lora_sdr.whitening
            def _whiten_compat(*a, **kw):
                if len(a) == 1 and isinstance(a[0], bool):
                    return _orig_whiten(a[0], False, ',', 'packet_len')
                return _orig_whiten(*a, **kw)
            lora_sdr.whitening = _whiten_compat
        except Exception:
            pass
        txrx_mod = importlib.import_module('tx_rx_simulation')
        # Instantiate reference top block
        self.tb = txrx_mod.tx_rx_simulation(payload_path, out_rx_payload, out_rx_crc,
                                            False, False, float(snr_db), int(samp_rate_hz), int(bw_hz), 868.1,
                                            int(sf), int(cr_api), int(os.path.getsize(payload_path)), float(clk_ppm), 1 if sf>6 else 0,
                                            int(preamb_len))
        # Keep channel handle for tapping IQ and CFO
        self.chan = self.tb.channels_channel_model_0

    def start(self):
        self.tb.start()

    def stop_wait(self):
        self.tb.stop(); self.tb.wait()

    def set_cfo(self, norm_cfo):
        try:
            self.chan.set_frequency_offset(float(norm_cfo))
        except Exception:
            pass


def run_once(sf: int, cr_lora: int, payload_path: str,
             out_iq_path: str, out_rx_payload: str, out_rx_crc: str,
             bw_hz: int, samp_rate_hz: int,
             snr_db: float, clk_ppm: float, cfo_hz: float,
             preamb_len: int, timeout: float) -> dict:
    from gnuradio import blocks, gr

    # Convert LoRa CR (45..48) -> internal (1..4)
    if cr_lora not in (45, 46, 47, 48):
        raise ValueError('cr must be 45/46/47/48')
    cr_api = cr_lora - 40
    pay_len = os.path.getsize(payload_path)

    # Prepare ASCII-hex payload file with trailing separator for whitening(file) path
    ascii_path = out_rx_payload + '.txascii'
    try:
        with open(payload_path, 'rb') as f_in, open(ascii_path, 'w') as f_out:
            data = f_in.read()
            hexstr = ''.join(f'{b:02x}' for b in data)
            f_out.write(hexstr + ',')  # trailing comma marks frame end
    except Exception as e:
        return {'ok': False, 'reason': f'prep_ascii_failed: {e}', 'rx_payload_len': 0}

    # Build local chain
    orig = OrigTxRx(sf, cr_api, ascii_path, out_rx_payload, out_rx_crc,
                    int(bw_hz), int(samp_rate_hz), float(snr_db), float(clk_ppm), int(preamb_len))

    # Tap channel output IQ (after impairments); connect additional sink
    from gnuradio import blocks, gr
    iq_sink = blocks.file_sink(gr.sizeof_gr_complex, out_iq_path, False)
    try:
        iq_sink.set_unbuffered(True)
    except Exception:
        pass
    # We can't directly connect without access; rewire: add headless connection using reference to existing channel
    # Instead, rely on file sink at frame_sync input by inserting a copy connection
    try:
        orig.tb.connect((orig.chan, 0), (iq_sink, 0))
    except Exception:
        pass

    # Apply CFO
    try:
        orig.set_cfo(float(cfo_hz) / float(samp_rate_hz))
    except Exception:
        pass

    # Run once until we either get CRC or timeout
    ok = False
    reason = 'timeout'
    try:
        orig.start()
        start = time.time()
        last_crc_sz = -1
        while True:
            try:
                crc_sz = os.path.getsize(out_rx_crc)
            except FileNotFoundError:
                crc_sz = 0
            if crc_sz != last_crc_sz:
                last_crc_sz = crc_sz
            if crc_sz > 0:
                break
            if time.time() - start > timeout:
                break
            time.sleep(0.05)
    finally:
        try:
            orig.stop_wait()
        except Exception:
            pass

    # Evaluate result
    if os.path.exists(out_rx_crc) and os.path.getsize(out_rx_crc) > 0:
        with open(out_rx_crc, 'rb') as f:
            flag = f.read(1)
            if flag:
                ok = (flag[0] != 0)
                reason = 'crc_ok' if ok else 'crc_fail'
    else:
        reason = 'no_crc'

    return {
        'ok': bool(ok),
        'reason': reason,
        'rx_payload_len': os.path.getsize(out_rx_payload) if os.path.exists(out_rx_payload) else 0,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sf', type=int, required=True)
    ap.add_argument('--cr', type=int, required=True)
    ap.add_argument('--payload', required=True)
    ap.add_argument('--out-iq', required=True)
    ap.add_argument('--out-rx-payload', required=True)
    ap.add_argument('--out-rx-crc', required=True)
    ap.add_argument('--bw', type=int, default=125000)
    ap.add_argument('--samp-rate', type=int, default=125000)
    ap.add_argument('--snr', type=float, default=0.0)
    ap.add_argument('--clk-ppm', type=float, default=0.0)
    ap.add_argument('--cfo-hz', type=float, default=0.0)
    ap.add_argument('--preamble-len', type=int, default=8)
    ap.add_argument('--timeout', type=float, default=20.0)
    args = ap.parse_args()

    try:
        import gnuradio, gnuradio.lora_sdr  # noqa: F401
    except Exception as e:
        print(json.dumps({'ok': False, 'reason': f'gnuradio_missing: {e}'}))
        return 2

    try:
        res = run_once(args.sf, args.cr, args.payload,
                       args.out_iq, args.out_rx_payload, args.out_rx_crc,
                       args.bw, args.samp_rate,
                       args.snr, args.clk_ppm, args.cfo_hz,
                       args.preamble_len, args.timeout)
        print(json.dumps(res))
        return 0
    except Exception as e:
        print(json.dumps({'ok': False, 'reason': f'exception: {e}'}))
        return 3


if __name__ == '__main__':
    sys.exit(main())
