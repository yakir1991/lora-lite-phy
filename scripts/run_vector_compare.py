#!/usr/bin/env python3
import argparse, json, os, subprocess, sys, shlex
from pathlib import Path


def run(cmd: list[str], cwd: str | None = None) -> tuple[int, str, str]:
    p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate()
    return p.returncode, out, err


def ensure_logs_dir() -> Path:
    p = Path('logs')
    p.mkdir(exist_ok=True)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in-iq', default='vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown')
    ap.add_argument('--sf', type=int, default=7)
    ap.add_argument('--cr', type=int, default=45)
    ap.add_argument('--bw', type=int, default=125000)
    ap.add_argument('--samp-rate', type=int, default=250000)
    ap.add_argument('--sync', type=lambda x:int(x,0), default=0x12)
    ap.add_argument('--pay-len', type=int, default=255)
    args = ap.parse_args()

    logs = ensure_logs_dir()
    vector = args.in_iq

    # 1) LoRa Lite
    lite_json = logs / 'lite_ld.json'
    lite_err = logs / 'lite_ld.err'
    lite_cmd = [
        './build/lora_decode',
        '--in', vector,
        '--sf', str(args.sf),
        '--cr', str(args.cr),
        '--sync', hex(args.sync),
        '--json',
    ]
    print('Running LoRa Lite:', ' '.join(shlex.quote(x) for x in lite_cmd))
    lite_rc, lite_out, lite_err_txt = run(lite_cmd)
    lite_json.write_text(lite_out)
    lite_err.write_text(lite_err_txt)
    lite_obj = None
    try:
        lines = [ln for ln in lite_out.splitlines() if ln.strip().startswith('{') and ln.strip().endswith('}')]
        lite_obj = json.loads(lines[-1]) if lines else None
    except Exception:
        lite_obj = None

    # 2) GNU Radio LoRa SDR
    gr_json_path = logs / 'gr_rx_only.json'
    # Prefer gnuradio-lora environment python if available
    gr_py = os.environ.get('GR_PYTHON')
    if not gr_py:
        cand = Path.home() / 'miniconda3' / 'envs' / 'gnuradio-lora' / 'bin' / 'python'
        if cand.exists():
            gr_py = str(cand)
        else:
            gr_py = sys.executable
    gr_cmd = [
        gr_py, 'scripts/gr_original_rx_only.py',
        '--in-iq', vector,
        '--sf', str(args.sf), '--cr', str(args.cr),
        '--bw', str(args.bw), '--samp-rate', str(args.samp_rate),
        '--pay-len', str(args.pay_len), '--sync', hex(args.sync),
        '--out-rx-payload', str(logs / 'gr_rx_payload.bin'),
        '--out-predew', str(logs / 'gr_predew.bin'),
        '--out-postdew', str(logs / 'gr_postdew.bin'),
        '--out-hdr-gray', str(logs / 'gr_hdr_gray.bin'),
        '--out-hdr-nibbles', str(logs / 'gr_hdr_nibbles.bin'),
    ]
    print('Running GNU Radio LoRa SDR:', ' '.join(shlex.quote(x) for x in gr_cmd))
    gr_rc, gr_out, gr_err = run(gr_cmd)
    gr_json_path.write_text(gr_out if gr_out else '')
    gr_obj = None
    try:
        glines = [ln for ln in gr_out.splitlines() if ln.strip().startswith('{') and ln.strip().endswith('}')]
        gr_obj = json.loads(glines[-1]) if glines else None
    except Exception:
        gr_obj = None

    result = {
        'lite': {
            'rc': lite_rc,
            'json': lite_obj,
        },
        'gr': {
            'rc': gr_rc,
            'json': gr_obj,
        }
    }
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
