#!/usr/bin/env python3
import argparse
import sys, os, json


def run_rx_only(in_iq: str, sf: int, cr_lora: int, bw_hz: int, samp_rate_hz: int, pay_len: int, timeout: float,
                out_rx_payload: str, sync_word: int,
                out_predew: str = None, out_postdew: str = None,
                out_hdr_gray: str = None, out_hdr_nibbles: str = None) -> dict:
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
                               sync_word=[int(sync_word)], soft_decoding=False, ldro_mode=2, print_rx=[False, False])
    sink_rx_payload = blocks.file_sink(gr.sizeof_char, out_rx_payload, False)
    # Optional taps: pre-dewhitening stream is output of header_decoder (nibbles, 1 byte per nibble),
    # we capture to a temporary .nib file and convert to bytes after the flow stops.
    nib_path = (out_predew + ".nib") if out_predew else None
    sink_predew = blocks.file_sink(gr.sizeof_char, nib_path, False) if out_predew else None
    # Post-dewhitening bytes are emitted by the dewhitening block directly
    sink_postdew = blocks.file_sink(gr.sizeof_char, out_postdew, False) if out_postdew else None
    # Header-level taps: gray-coded symbols (from gray_mapping), and Hamming-decoded nibbles (from hamming_dec)
    # gray_mapping outputs 16-bit symbols → use sizeof_short sink
    sink_hdr_gray = blocks.file_sink(gr.sizeof_short, out_hdr_gray, False) if out_hdr_gray else None
    sink_hdr_nibbles = blocks.file_sink(gr.sizeof_char, out_hdr_nibbles, False) if out_hdr_nibbles else None
    tb.connect(src, rx)
    tb.connect(rx, sink_rx_payload)
    if sink_predew:
        tb.connect(rx.lora_sdr_header_decoder_0, sink_predew)
    if sink_postdew:
        tb.connect(rx.lora_sdr_dewhitening_0, sink_postdew)
    if sink_hdr_gray:
        tb.connect(rx.lora_sdr_gray_mapping_0, sink_hdr_gray)
    if sink_hdr_nibbles:
        tb.connect(rx.lora_sdr_hamming_dec_0, sink_hdr_nibbles)

    # Run the flowgraph to completion (file source is finite). Avoid waiting on CRC-dependent payload.
    try:
        tb.start(); tb.wait()
    finally:
        try:
            tb.stop()
        except Exception:
            pass

    # If requested, convert captured nibbles to bytes for pre-dewhitening reference
    if out_predew:
        try:
            with open(nib_path, 'rb') as f:
                nib = f.read()
        except FileNotFoundError:
            nib = b''
        # Group into pairs: [low, high] -> byte
        outb = bytearray()
        for i in range(0, len(nib) - 1, 2):
            low = nib[i] & 0x0F
            high = nib[i+1] & 0x0F
            outb.append((high << 4) | low)
        try:
            with open(out_predew, 'wb') as f:
                f.write(outb)
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
    ap.add_argument('--out-predew')
    ap.add_argument('--out-postdew')
    ap.add_argument('--sync', type=lambda x: int(x, 0), default=0x34)
    ap.add_argument('--out-hdr-gray')
    ap.add_argument('--out-hdr-nibbles')
    args = ap.parse_args()

    try:
        import gnuradio, gnuradio.lora_sdr  # noqa: F401
    except Exception as e:
        print(json.dumps({'ok': False, 'reason': f'gnuradio_missing: {e}'}))
        return 2

    try:
        res = run_rx_only(args.in_iq, args.sf, args.cr, args.bw, args.samp_rate, args.pay_len, args.timeout,
                          args.out_rx_payload, args.sync, args.out_predew, args.out_postdew,
                          args.out_hdr_gray, args.out_hdr_nibbles)
        print(json.dumps(res))
        return 0
    except Exception as e:
        print(json.dumps({'ok': False, 'reason': f'exception: {e}'}))
        return 3


if __name__ == '__main__':
    sys.exit(main())
