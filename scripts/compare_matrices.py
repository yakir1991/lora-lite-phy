#!/usr/bin/env python3
import argparse
from pathlib import Path

def load_csv(csv: Path):
    rows = {}
    if not csv.exists():
        return rows
    lines = csv.read_text().splitlines()
    for i, line in enumerate(lines):
        if not line.strip():
            continue
        if i == 0 and line.startswith('sf,'):
            continue
        c = line.split(',')
        if len(c) < 7:
            c += [''] * (7 - len(c))
        sf, cr, bw, ln, snr, ok, reason = c[:7]
        key = (int(sf), int(cr), int(bw), int(ln), float(snr))
        rows[key] = {'ok': int(ok), 'reason': reason}
    return rows

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--orig', required=True)
    ap.add_argument('--lite', required=True)
    ap.add_argument('--outdir', required=True)
    args = ap.parse_args()
    orig = load_csv(Path(args.orig))
    lite = load_csv(Path(args.lite))
    keys = sorted(set(orig.keys()) | set(lite.keys()))
    only_orig = [k for k in keys if k in orig and k not in lite]
    only_lite = [k for k in keys if k in lite and k not in orig]
    mism = [k for k in keys if k in orig and k in lite and orig[k]['ok'] != lite[k]['ok']]
    both_fail = [k for k in keys if k in orig and k in lite and orig[k]['ok'] == 0 and lite[k]['ok'] == 0]
    # Write report
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    rd = outdir / 'README.md'
    lines = []
    lines.append('# Matrix Compare: Original vs LoRa Lite')
    lines.append('')
    lines.append(f'- Cases (union): {len(keys)}')
    lines.append(f'- Only in original: {len(only_orig)}')
    lines.append(f'- Only in lite: {len(only_lite)}')
    lines.append(f'- Mismatched success (orig!=lite): {len(mism)}')
    lines.append(f'- Both failed: {len(both_fail)}')
    if mism:
        lines.append('')
        lines.append('## Mismatches (sf,cr,bw,len,snr)')
        for k in mism[:100]:
            lines.append(f'- {k}: orig ok={orig[k]["ok"]} ({orig[k]["reason"]}), lite ok={lite[k]["ok"]} ({lite[k]["reason"]})')
        if len(mism) > 100:
            lines.append(f'- ... and {len(mism)-100} more')
    rd.write_text('\n'.join(lines) + '\n')
    print(f'Wrote compare to {rd}')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

