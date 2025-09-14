#!/usr/bin/env python3
import argparse
import json
import subprocess
from pathlib import Path


def run_hdr_scan(build_dir: Path, in_iq: Path, sf: int, cr: int, sync: str, min_pre: int,
                 off0: int, off0_span: int, off1: int, off1_span: int, fine_radius: int) -> Path:
    out_json = Path('logs/lite_hdr_scan.json')
    cmd = [
        str(build_dir / 'lora_decode'), '--in', str(in_iq), '--sf', str(sf), '--cr', str(cr),
        '--sync', sync, '--min-preamble', str(min_pre), '--hdr-scan', '--hdr-scan-fine',
        '--hdr-off0', str(off0), '--hdr-off0-span', str(off0_span),
        '--hdr-off1', str(off1), '--hdr-off1-span', str(off1_span),
        '--hdr-fine-radius', str(fine_radius)
    ]
    print('[auto] Running hdr-scan...')
    print('[auto]  cmd:', ' '.join(cmd))
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        print('[auto][hdr-scan] stderr:\n' + proc.stderr)
        raise SystemExit(f'hdr-scan failed with code {proc.returncode}')
    if proc.stderr:
        # summarize lora_decode stderr (scan stats)
        for line in proc.stderr.splitlines()[-5:]:
            print('[auto][hdr-scan]', line)
    if not out_json.exists():
        raise SystemExit('hdr-scan did not create logs/lite_hdr_scan.json')
    sz = out_json.stat().st_size
    print(f'[auto] hdr-scan produced {out_json} ({sz} bytes)')
    return out_json


def parse_best(from_script: Path, scan_json: Path) -> dict:
    # Reuse existing analyzer to print best; capture stdout and parse simple lines
    print('[auto] Analyzing scan JSON with from_lite_dbg_hdr_to_cw.py...')
    print('[auto]  cmd:', 'python3', str(from_script), '--in', str(scan_json))
    proc = subprocess.run(['python3', str(from_script), '--in', str(scan_json)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        print('[auto][analyze] stderr:\n' + proc.stderr)
        raise SystemExit(f'analyzer failed with code {proc.returncode}')
    out = proc.stdout.splitlines()
    best = {
        'best_timing': None,
        'best_variant': None,
    }
    for i, line in enumerate(out):
        if line.strip().startswith('Best across hdr-scan (timing only):'):
            # Next lines contain params and cw lines
            # Expect:  best params: {'off0': 2, 'samp0': 0, 'off1': -1, 'samp1': 0}
            for j in range(1, 6):
                if i + j < len(out) and 'best params:' in out[i + j]:
                    s = out[i + j].split('best params:', 1)[1].strip()
                    best['best_timing'] = eval(s, {}, {})
                    break
        if line.strip().startswith('Best with block1 variants'):
            # Expect params and variant description within next few lines
            params = None
            diag = {}
            for j in range(1, 8):
                if i + j >= len(out):
                    break
                L = out[i + j].strip()
                if L.startswith('params:'):
                    params = eval(L.split('params:', 1)[1].strip(), {}, {})
                if L.startswith('diagshift=') or 'rot1=' in L:
                    # crude parse key=value pairs
                    parts = L.replace(',', ' ').split()
                    for p in parts:
                        if '=' in p:
                            k, v = p.split('=', 1)
                            try:
                                diag[k] = int(v)
                            except ValueError:
                                diag[k] = v
            if params:
                best['best_variant'] = {'params': params, 'variant': diag}
    return best


def main():
    ap = argparse.ArgumentParser(description='Auto-find header timing for LoRa Lite via hdr-scan')
    ap.add_argument('--build', default='build')
    ap.add_argument('--in', dest='in_iq', required=True)
    ap.add_argument('--sf', type=int, required=True)
    ap.add_argument('--cr', type=int, required=True)
    ap.add_argument('--sync', default='0x12')
    ap.add_argument('--min-pre', type=int, default=8)
    ap.add_argument('--off0', type=int, default=2)
    ap.add_argument('--off0-span', type=int, default=3)
    ap.add_argument('--off1', type=int, default=0)
    ap.add_argument('--off1-span', type=int, default=6)
    ap.add_argument('--fine-radius', type=int, default=64)
    ap.add_argument('--out', default='logs/auto_timing.json')
    args = ap.parse_args()

    build_dir = Path(args.build)
    in_iq = Path(args.in_iq)
    from_script = Path('scripts/from_lite_dbg_hdr_to_cw.py')

    print('[auto] Args summary:', {
        'sf': args.sf, 'cr': args.cr, 'sync': args.sync, 'min_pre': args.min_pre,
        'off0': args.off0, 'off0_span': args.off0_span,
        'off1': args.off1, 'off1_span': args.off1_span, 'fine_radius': args.fine_radius,
    })

    scan_json = run_hdr_scan(build_dir, in_iq, args.sf, args.cr, args.sync, args.min_pre,
                             args.off0, args.off0_span, args.off1, args.off1_span, args.fine_radius)
    best = parse_best(from_script, scan_json)
    print('[auto] Best timing only:', best.get('best_timing'))
    print('[auto] Best with block1 variant:', best.get('best_variant'))
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(best, indent=2))
    print('[auto] Wrote JSON to', args.out)
    # Print a ready-to-run hdr-single command suggestion if timing available
    bt = best.get('best_timing') or {}
    if {'off0','samp0','off1','samp1'} <= bt.keys():
        print('[auto] Suggested hdr-single:')
        print('  --hdr-off0-single {off0} --hdr-samp0 {samp0} --hdr-off1-single {off1} --hdr-samp1 {samp1}'.format(**bt))
    else:
        print('[auto] Warning: best_timing not found. Consider increasing scan spans.')


if __name__ == '__main__':
    main()
