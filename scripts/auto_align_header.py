#!/usr/bin/env python3
import argparse
import json
import subprocess
from pathlib import Path

def sh(cmd, check=True):
    print('[auto-align] $', ' '.join(cmd))
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if check and p.returncode != 0:
        print('[auto-align][stderr]\n' + p.stderr)
        raise SystemExit(f'command failed: {p.returncode}')
    return p

def hdr_scan(build, in_iq, sf, cr, sync, min_pre, off0, off0_span, off1, off1_span, fine_radius):
    cmd = [
        str(Path(build) / 'lora_decode'), '--in', str(in_iq), '--sf', str(sf), '--cr', str(cr),
        '--sync', sync, '--min-preamble', str(min_pre), '--hdr-scan', '--hdr-scan-fine',
        '--hdr-off0', str(off0), '--hdr-off0-span', str(off0_span),
        '--hdr-off1', str(off1), '--hdr-off1-span', str(off1_span),
        '--hdr-fine-radius', str(fine_radius)
    ]
    p = sh(cmd, check=True)
    print('[auto-align] hdr-scan done. logs/lite_hdr_scan.json size:', Path('logs/lite_hdr_scan.json').stat().st_size)


def parse_best():
    p = sh(['python3', 'scripts/from_lite_dbg_hdr_to_cw.py', '--in', 'logs/lite_hdr_scan.json'])
    best = {'best_timing': None, 'best_variant': None}
    for i, line in enumerate(p.stdout.splitlines()):
        s = line.strip()
        if s.startswith('best params:'):
            best['best_timing'] = eval(s.split('best params:',1)[1].strip(), {}, {})
        if s.startswith('params:') and best.get('best_variant') is None:
            params = eval(s.split('params:',1)[1].strip(), {}, {})
            best['best_variant'] = {'params': params}
    print('[auto-align] best:', best)
    return best

def hdr_single(build, in_iq, sf, cr, sync, min_pre, off0, samp0, off1, samp1, tag):
    err = Path(f'logs/hdr_single_{tag}.err')
    out = Path(f'logs/hdr_single_{tag}.out')
    cmd = [
        str(Path(build) / 'lora_decode'), '--in', str(in_iq), '--sf', str(sf), '--cr', str(cr),
        '--sync', sync, '--min-preamble', str(min_pre), '--hdr-single',
        '--hdr-off0-single', str(off0), '--hdr-samp0', str(samp0),
        '--hdr-off1-single', str(off1), '--hdr-samp1', str(samp1)
    ]
    p = sh(cmd, check=False)
    out.write_text(p.stdout)
    err.write_text(p.stderr)
    print(f'[auto-align] hdr-single[{tag}] wrote {err}')
    return p

def anchor_probe(build, in_iq, sf, cr, sync, min_pre, base_off0, base_samp0, base_off1, base_samp1):
    log = Path('logs/hdr_anchor_auto_grid.out')
    log.write_text('')
    best = {'diff': 1e9, 'off_sym': 0, 'off_samp': 0}
    for so in [-2,-1,0,1,2]:
        for sa in [-96,-64,-48,-32,-16,0,16,32,48,64,96]:
            print(f'[auto-align] probe SYM={so} SAMP={sa}')
            env = {
                'LORA_HDR_BASE_SYM_OFF': str(so),
                'LORA_HDR_BASE_SAMP_OFF': str(sa),
            }
            cmd = [
                str(Path(build) / 'lora_decode'), '--in', str(in_iq), '--sf', str(sf), '--cr', str(cr),
                '--sync', sync, '--min-preamble', str(min_pre), '--hdr-single',
                '--hdr-off0-single', str(base_off0), '--hdr-samp0', str(base_samp0),
                '--hdr-off1-single', str(base_off1), '--hdr-samp1', str(base_samp1)
            ]
            p = subprocess.run(cmd, env={**env, **dict(**{})}, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            with log.open('a') as f:
                f.write(f'=== SYM={so} SAMP_OFF={sa} ===\n')
                f.write(p.stderr)
            # quick parse of diff from stderr using existing grid parser later
    # parse grid
    pr = sh(['python3', 'scripts/parse_hdr_single_grid.py', '--in', str(log), '--top', '20'], check=False)
    Path('logs/hdr_anchor_auto_grid_parsed.out').write_text(pr.stdout)
    print('[auto-align] anchor grid parsed (top 20) written to logs/hdr_anchor_auto_grid_parsed.out')


def full_decode(build, in_iq, sf, cr, sync, min_pre):
    p = sh([str(Path(build)/'lora_decode'), '--in', str(in_iq), '--sf', str(sf), '--cr', str(cr), '--sync', sync, '--min-preamble', str(min_pre), '--json'], check=False)
    Path('logs/auto_full.json').write_text(p.stdout)
    Path('logs/auto_full.err').write_text(p.stderr)
    print('[auto-align] full decode wrote logs/auto_full.json, logs/auto_full.err')


def main():
    ap = argparse.ArgumentParser(description='Auto-align LoRa header: scan, anchor, hdr-single, decode')
    ap.add_argument('--build', default='build')
    ap.add_argument('--in', dest='in_iq', required=True)
    ap.add_argument('--sf', type=int, required=True)
    ap.add_argument('--cr', type=int, required=True)
    ap.add_argument('--sync', default='0x12')
    ap.add_argument('--min-pre', type=int, default=8)
    ap.add_argument('--out', default='logs/auto_align_report.json')
    args = ap.parse_args()

    build = args.build; in_iq = args.in_iq
    print('[auto-align] args:', vars(args))

    # 1) hdr-scan
    hdr_scan(build, in_iq, args.sf, args.cr, args.sync, args.min_pre, off0=2, off0_span=3, off1=0, off1_span=6, fine_radius=64)

    # 2) parse best
    best = parse_best()
    bt = best.get('best_timing') or {}
    bv = (best.get('best_variant') or {}).get('params', {})

    # 3) hdr-single at both suggested spots
    res = {'best_timing': bt, 'best_variant': bv}
    if {'off0','samp0','off1','samp1'} <= bt.keys():
        print('[auto-align] try hdr-single at best_timing')
        p = hdr_single(build, in_iq, args.sf, args.cr, args.sync, args.min_pre, bt['off0'], bt['samp0'], bt['off1'], bt['samp1'], tag='best')
        res['hdr_single_best'] = {'rc': p.returncode}
    if {'off0','samp0','off1','samp1'} <= bv.keys():
        print('[auto-align] try hdr-single at best_variant params')
        p = hdr_single(build, in_iq, args.sf, args.cr, args.sync, args.min_pre, bv['off0'], bv['samp0'], bv['off1'], bv['samp1'], tag='variant')
        res['hdr_single_variant'] = {'rc': p.returncode}

    # 4) anchor probe around best timing
    if {'off0','samp0','off1','samp1'} <= bt.keys():
        anchor_probe(build, in_iq, args.sf, args.cr, args.sync, args.min_pre, bt['off0'], bt['samp0'], bt['off1'], bt['samp1'])

    # 5) full decode
    full_decode(build, in_iq, args.sf, args.cr, args.sync, args.min_pre)

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(res, indent=2))
    print('[auto-align] wrote report to', args.out)


if __name__ == '__main__':
    main()
