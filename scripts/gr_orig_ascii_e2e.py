#!/usr/bin/env python3
# Unified to use the hierarchical TX/RX harness (stable path)
# Delegates to scripts/gr_original_e2e.py under the hood.
import argparse, json, subprocess, sys, pathlib


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
    # Back-compat placeholders (not used by hierarchical harness)
    ap.add_argument('--rx-payload', default='/tmp/gr_orig_ascii_rx.bin')
    ap.add_argument('--rx-crc', default='/tmp/gr_orig_ascii_crc.bin')
    args = ap.parse_args()

    root = pathlib.Path(__file__).resolve().parents[1]
    e2e = root / 'scripts' / 'gr_original_e2e.py'

    # Delegate to hierarchical E2E runner. It prints a JSON with ok/sent/recv.
    cmd = [sys.executable, str(e2e), '--sf', str(args.sf), '--cr', str(args.cr), '--len', str(args.len),
           '--bw', str(args.bw), '--samp-rate', str(args.samp_rate), '--snr', str(args.snr),
           '--clk-ppm', str(args.clk_ppm), '--preamble-len', str(args.preamble_len), '--timeout', str(args.timeout),
           '--seed', str(args.seed), '--out-iq', str(args.out_iq)]
    res = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = res.stdout.strip()
    try:
        j = json.loads(out) if out else {}
    except Exception:
        j = {'ok': False, 'reason': 'bad_json'}
    print(json.dumps({'ok': bool(j.get('ok', False)), 'sent': j.get('sent',''), 'recv': j.get('recv','')}))
    return 0 if bool(j.get('ok', False)) else 1


if __name__ == '__main__':
    raise SystemExit(main())
